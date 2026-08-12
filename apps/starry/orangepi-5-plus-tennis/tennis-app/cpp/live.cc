// SPDX-License-Identifier: Apache-2.0
//
// Live pipeline + test modes for the board.
//
// Threading: libuvc runs its own capture thread and latches only the newest
// frame; this file's main loop polls that latest-frame slot, runs
// perception (YOLO or HSV, per the FSM's current mode), and the control step.
// Virtual backends are non-blocking. Real UART backends intentionally preserve
// the reference controller's synchronous command/ACK and servo-settle behavior,
// so live frame-to-command timing includes actuator latency when they are used.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // sched_setaffinity / sched_getcpu / CPU_SET
#endif

#include "live.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <thread>

#include "bench/metrics.h"
#include "actuator/actuator_factory.h"
#include "image_utils.h" // read_image (fixed-image validation)
#include "profiling.h"    // apply_and_check_affinity / profiler_getcpu
#include "controller.h"
#include "perception/bucket_detector.h"
#include "perception/camera.h"
#include "perception/tennis_detector.h"
#include "actuator/arm_backend.h"
#include "actuator/motor_backend.h"
#include "time_utils.h"
#include "types.h"

#include "uvc_capture.h" // frame_to_image, LatestFrame, image_buffer_t

namespace tennis {

static constexpr int64_t kIdleSleepNs = 1000000; // 1 ms when no new frame

volatile sig_atomic_t g_termination_requested = 0;

void request_termination(int) {
    g_termination_requested = 1;
}

class TerminationSignals {
public:
    TerminationSignals() {
        g_termination_requested = 0;
        struct sigaction action {};
        action.sa_handler = request_termination;
        sigemptyset(&action.sa_mask);
        int_installed_ = sigaction(SIGINT, &action, &old_int_) == 0;
        term_installed_ = sigaction(SIGTERM, &action, &old_term_) == 0;
    }

    ~TerminationSignals() {
        if (int_installed_) sigaction(SIGINT, &old_int_, nullptr);
        if (term_installed_) sigaction(SIGTERM, &old_term_, nullptr);
    }

    bool requested() const { return g_termination_requested != 0; }

private:
    struct sigaction old_int_ {};
    struct sigaction old_term_ {};
    bool int_installed_ = false;
    bool term_installed_ = false;
};

static Config make_cfg(const Options &opts) {
    Config cfg = opts.cfg;
    cfg.frame_w = opts.width;
    cfg.frame_h = opts.height;
    return cfg;
}

int run_live(const Options &opts) {
    // Cold-start clock: process start ~= run_live entry (arg parse is trivial).
    const int64_t t_proc_start = monotonic_ns();
    std::printf("TENNIS_PROC_START\n");
    std::fflush(stdout);
    Config cfg = make_cfg(opts);
    TerminationSignals termination;

    Actuators actuators;
    if (!make_actuators(opts, actuators)) return 1;
    if (opts.arm_backend != "virtual") {
        if (!actuators.arm->ready()) {
            std::fprintf(stderr, "TENNIS_ERROR initial arm homing failed\n");
            return 1;
        }
    }

    // Camera open/negotiation uses USB control transfers and can overlap RKNN
    // initialization. Actual streaming starts only after rknn_init: starting it
    // earlier floods xHCI completions and substantially delays the model's
    // producer/consumer handshakes on StarryOS.
    Camera cam;
    TennisDetector det;
    bool camera_open_ok = false;
    int64_t t_cap_open0 = 0;
    int64_t t_cap_open1 = 0;
    std::thread camera_setup([&] {
        t_cap_open0 = monotonic_ns();
        camera_open_ok = cam.open_and_negotiate(
            opts.device, opts.width, opts.height, opts.fps);
        t_cap_open1 = monotonic_ns();
    });

    const int64_t t_mdl0 = monotonic_ns();
    const bool model_ok =
        det.init(opts.model.c_str(), opts.label.c_str(), cfg, opts.core_mask);
    const int64_t t_mdl1 = monotonic_ns();
    camera_setup.join();

    if (!model_ok) {
        std::fprintf(stderr, "rknn_init fail! model=%s\n", opts.model.c_str());
        return 1;
    }
    if (!camera_open_ok) {
        std::fprintf(stderr, "uvc open/negotiate failed: device %d\n",
                     opts.device);
        return 1;
    }
    const int64_t t_cap_stream0 = monotonic_ns();
    if (!cam.begin_streaming()) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }
    const int64_t t_cap1 = monotonic_ns();
    const double capture_init_ms =
        ns_to_ms(t_cap_open1 - t_cap_open0) +
        ns_to_ms(t_cap1 - t_cap_stream0);

    if (!cam.warm_up(opts.camera_warmup_frames,
                     opts.camera_warmup_timeout_ms)) {
        std::fprintf(stderr,
                     "TENNIS_ERROR camera warmup timed out after %d ms\n",
                     opts.camera_warmup_timeout_ms);
        return 1;
    }
    if (termination.requested()) return 0;

    BucketDetector bucket(cfg);
    Metrics metrics;
    metrics.reserve(static_cast<size_t>(opts.duration_sec * opts.fps) + 16);
    Controller controller(cfg, *actuators.motor, *actuators.arm, metrics,
                          opts.log_every);

    const bool do_csv = opts.profile && !opts.profile_csv.empty();
    if (do_csv && !metrics.open_csv(opts.profile_csv.c_str())) {
        std::fprintf(stderr, "TENNIS_WARN could not open profile-csv %s\n",
                     opts.profile_csv.c_str());
    }

    std::printf("TENNIS_BENCH_BEGIN mode=live model=%s fps=%d duration_sec=%.3f "
                "core_mask=%s virtual_actuators=%d motor_backend=%s "
                "arm_backend=%s profile=%d affinity=%s\n",
                opts.model.c_str(), opts.fps, opts.duration_sec,
                opts.core_mask.c_str(), uses_virtual_actuators(opts) ? 1 : 0,
                opts.motor_backend.c_str(), opts.arm_backend.c_str(),
                opts.profile ? 1 : 0,
                opts.infer_affinity.empty() ? "none"
                                            : opts.infer_affinity.c_str());

    // Pin the inference loop AFTER model + camera init (NPU init hangs on a
    // non-boot core); the libuvc capture thread was spawned during cam.start and
    // stays on its core, only this loop migrates.
    if (!opts.infer_affinity.empty()) {
        apply_and_check_affinity(opts.infer_affinity.c_str());
    }

    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t capture_ts = 0;
    int64_t last_report = start;
    int64_t last_frame = start;
    int64_t t_first_frame = 0, t_first_detect = 0, t_first_cmd = 0;
    bool actuator_failed = false;
    // One persistent RGB buffer reused by frame_to_image every frame (avoids a
    // malloc/free + page-zero per frame); freed once after the loop.
    image_buffer_t img{};

    while (!termination.requested() && monotonic_ns() < end) {
        if (!cam.poll(lf, capture_ts)) {
            const int64_t now = monotonic_ns();
            if (!controller.tick(now)) {
                actuator_failed = true;
                break;
            }
            if (now - last_frame >
                static_cast<int64_t>(opts.camera_watchdog_ms) * 1000000) {
                std::fprintf(stderr,
                             "TENNIS_ERROR camera produced no frame for %d ms\n",
                             opts.camera_watchdog_ms);
                actuator_failed = true;
                break;
            }
            sleep_ns(kIdleSleepNs);
            continue;
        }
        last_frame = monotonic_ns();

        const int64_t pickup = monotonic_ns();
        const double queue_age_ms = ns_to_ms(pickup - capture_ts);

        // Drop frames already older than the staleness bound (0 = never).
        if (cfg.staleness_ms > 0 &&
            queue_age_ms > static_cast<double>(cfg.staleness_ms)) {
            continue;
        }
        if (t_first_frame == 0) t_first_frame = pickup;

        StageTiming st;
        st.seq = lf.id;
        st.queue_age_ms = queue_age_ms;

        Detection d;
        d.seq = lf.id;
        d.capture_ts_ns = capture_ts;
        d.mode = controller.perception_mode();
        d.valid = true;

        if (d.mode == PerceptionMode::BALL) {
            // Zero-copy preprocess: the detector converts the raw camera frame
            // (YUYV via RGA, MJPEG via CPU) straight into its NPU input buffer --
            // no separate frame_to_image CPU decode. `decode_ms` now measures that
            // preprocess (reported as prof.letterbox_ms).
            rknn_inference_profile_t prof{};
            const int dr = det.detect(lf, d.ball, &prof);
            if (dr == TennisDetector::kShortFrame) {
                // Incomplete USB-ISO frame: dropped, not an inference error. Skip.
                metrics.on_short_frame();
                continue;
            }
            if (dr != 0) {
                metrics.on_inference_error();
            }
            st.decode_ms = prof.letterbox_ms;
            st.inputs_set_ms = prof.inputs_set_ms;
            st.run_ms = prof.run_ms;
            st.rknn_perf_run_ms = prof.rknn_perf_run_ms;
            st.outputs_get_ms = prof.outputs_get_ms;
            st.postprocess_ms = prof.postprocess_ms;
        } else {
            // Bucket (HSV) still needs a decoded RGB frame on the CPU.
            const int64_t t_dec0 = monotonic_ns();
            if (frame_to_image(lf, &img) != 0) {
                metrics.on_decode_error();
                continue;
            }
            st.decode_ms = ns_to_ms(monotonic_ns() - t_dec0);
            bucket.detect(&img, d.bucket);
        }
        d.detect_ts_ns = monotonic_ns();
        if (t_first_detect == 0) {
            t_first_detect = d.detect_ts_ns;
            std::printf("TENNIS_FIRST_INFERENCE ms_since_proc_start=%.1f\n",
                        ns_to_ms(t_first_detect - t_proc_start));
            std::fflush(stdout);
        }

        const int64_t t_ctrl0 = monotonic_ns();
        if (!controller.process(d)) {
            actuator_failed = true;
            break;
        }
        const int64_t t_ctrl1 = monotonic_ns();
        if (t_first_cmd == 0) t_first_cmd = t_ctrl1;

        if (opts.profile) {
            st.control_ms = ns_to_ms(t_ctrl1 - t_ctrl0);
            st.frame_to_detection_ms = ns_to_ms(d.detect_ts_ns - capture_ts);
            st.frame_to_command_ms = ns_to_ms(t_ctrl1 - capture_ts);
            st.detections =
                (d.mode == PerceptionMode::BALL ? d.ball.found : d.bucket.found)
                    ? 1
                    : 0;
            metrics.on_stage_timing(st);
            metrics.record_core(profiler_getcpu());

            if (opts.report_interval_sec > 0 &&
                ns_to_ms(t_ctrl1 - last_report) >=
                    opts.report_interval_sec * 1000.0) {
                metrics.sample_resource(ns_to_ms(t_ctrl1 - start) / 1000.0);
                last_report = t_ctrl1;
            }
        }
    }

    if (!actuators.motor->standby()) actuator_failed = true;
    if (termination.requested()) {
        std::fprintf(stderr, "TENNIS_INFO termination requested; actuators stopped\n");
    }

    if (img.virt_addr) std::free(img.virt_addr);

    const double dur = ns_to_ms(monotonic_ns() - start) / 1000.0;
    const UvcCaptureCounters cc = cam.counters();
    metrics.set_captured(cc.captured);

    if (opts.profile) {
        ColdStart cold;
        cold.capture_init_ms = capture_init_ms;
        cold.model_init_ms = ns_to_ms(t_mdl1 - t_mdl0);
        if (t_first_frame)
            cold.first_frame_wait_ms = ns_to_ms(t_first_frame - t_cap1);
        if (t_first_detect && t_first_frame)
            cold.first_detection_ms = ns_to_ms(t_first_detect - t_first_frame);
        if (t_first_cmd && t_first_detect)
            cold.first_command_ms = ns_to_ms(t_first_cmd - t_first_detect);
        if (t_first_cmd)
            cold.time_to_first_command_ms = ns_to_ms(t_first_cmd - t_proc_start);
        metrics.set_cold_start(cold);
    }

    metrics.emit_result(dur);
    if (opts.profile) {
        metrics.emit_profile_result();
        metrics.emit_pipeline_result();
        metrics.emit_cold_start();
        metrics.emit_core_hist(opts.infer_affinity.empty()
                                   ? "none"
                                   : opts.infer_affinity.c_str());
        metrics.close_csv();
    }
    std::printf("TENNIS_BENCH_DONE\n");

    cam.stop();
    det.deinit();
    return actuator_failed ? 1 : 0;
}

int run_test_uvc(const Options &opts) {
    Camera cam;
    const int64_t t_cap0 = monotonic_ns();
    if (!cam.start(opts.device, opts.width, opts.height, opts.fps)) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }
    const int64_t t_cap1 = monotonic_ns();

    Metrics metrics;
    metrics.reserve(static_cast<size_t>(opts.duration_sec * opts.fps) + 16);
    if (opts.profile && !opts.profile_csv.empty()) {
        metrics.open_csv(opts.profile_csv.c_str());
    }
    if (!opts.infer_affinity.empty()) {
        apply_and_check_affinity(opts.infer_affinity.c_str());
    }

    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t ts = 0;
    uint64_t polled = 0;
    int64_t last_report = start;
    int64_t t_first_frame = 0;
    while (monotonic_ns() < end) {
        if (cam.poll(lf, ts)) {
            ++polled;
            const int64_t now = monotonic_ns();
            if (t_first_frame == 0) t_first_frame = now;
            if (opts.profile) {
                StageTiming st;
                st.seq = lf.id;
                st.queue_age_ms = ns_to_ms(now - ts);
                metrics.on_stage_timing(st);
                metrics.record_core(profiler_getcpu());
                if (opts.report_interval_sec > 0 &&
                    ns_to_ms(now - last_report) >=
                        opts.report_interval_sec * 1000.0) {
                    metrics.sample_resource(ns_to_ms(now - start) / 1000.0);
                    last_report = now;
                }
            }
        } else {
            sleep_ns(kIdleSleepNs);
        }
    }
    const double dur = ns_to_ms(monotonic_ns() - start) / 1000.0;
    const UvcCaptureCounters cc = cam.counters();
    std::printf("TENNIS_TEST_UVC device=%d captured=%llu polled_new=%llu "
                "dropped=%llu bytes=%llu duration_sec=%.3f capture_fps=%.2f\n",
                opts.device, static_cast<unsigned long long>(cc.captured),
                static_cast<unsigned long long>(polled),
                static_cast<unsigned long long>(cc.dropped),
                static_cast<unsigned long long>(cc.bytes), dur,
                dur > 0 ? static_cast<double>(cc.captured) / dur : 0.0);
    if (opts.profile) {
        ColdStart cold;
        cold.capture_init_ms = ns_to_ms(t_cap1 - t_cap0);
        if (t_first_frame) cold.first_frame_wait_ms = ns_to_ms(t_first_frame - t_cap1);
        metrics.set_cold_start(cold);
        metrics.emit_pipeline_result();
        metrics.emit_cold_start();
        metrics.emit_core_hist(opts.infer_affinity.empty()
                                   ? "none"
                                   : opts.infer_affinity.c_str());
        metrics.close_csv();
    }
    std::printf("TENNIS_TEST_UVC_DONE\n");
    cam.stop();
    return 0;
}

int run_test_yolo(const Options &opts) {
    Config cfg = make_cfg(opts);
    Camera cam;
    const int64_t t_cap0 = monotonic_ns();
    if (!cam.start(opts.device, opts.width, opts.height, opts.fps)) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }
    const int64_t t_cap1 = monotonic_ns();
    TennisDetector det;
    const int64_t t_mdl0 = monotonic_ns();
    if (!det.init(opts.model.c_str(), opts.label.c_str(), cfg, opts.core_mask)) {
        std::fprintf(stderr, "rknn_init fail! model=%s\n", opts.model.c_str());
        cam.stop();
        return 1;
    }
    const int64_t t_mdl1 = monotonic_ns();

    Metrics metrics;
    metrics.reserve(static_cast<size_t>(opts.duration_sec * opts.fps) + 16);
    if (opts.profile && !opts.profile_csv.empty()) {
        metrics.open_csv(opts.profile_csv.c_str());
    }
    // Pin AFTER init (NPU init hangs on a non-boot core).
    if (!opts.infer_affinity.empty()) {
        apply_and_check_affinity(opts.infer_affinity.c_str());
    }

    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t ts = 0;
    uint64_t frames = 0, detections = 0, infer_errors = 0;
    int64_t last_report = start;
    int64_t t_first_frame = 0, t_first_detect = 0;
    while (monotonic_ns() < end) {
        if (!cam.poll(lf, ts)) {
            sleep_ns(kIdleSleepNs);
            continue;
        }
        const int64_t pickup = monotonic_ns();
        if (t_first_frame == 0) t_first_frame = pickup;
        StageTiming st;
        st.seq = lf.id;
        st.queue_age_ms = ns_to_ms(pickup - ts);

        // Zero-copy preprocess from the raw frame (no frame_to_image CPU decode).
        BallObs ball;
        rknn_inference_profile_t prof{};
        const int dr = det.detect(lf, ball, &prof);
        if (dr == TennisDetector::kShortFrame) {
            continue; // incomplete USB-ISO frame: dropped, not counted
        }
        if (dr != 0) {
            ++infer_errors;
        } else if (ball.found) {
            ++detections;
        }
        const int64_t t_det = monotonic_ns();
        if (t_first_detect == 0) t_first_detect = t_det;
        st.decode_ms = prof.letterbox_ms; // YUYV->RGB preprocess (RGA/CPU)
        st.inputs_set_ms = prof.inputs_set_ms;
        st.run_ms = prof.run_ms;
        st.rknn_perf_run_ms = prof.rknn_perf_run_ms;
        st.outputs_get_ms = prof.outputs_get_ms;
        st.postprocess_ms = prof.postprocess_ms;
        st.frame_to_detection_ms = ns_to_ms(t_det - ts);
        st.detections = ball.found ? 1 : 0;
        ++frames;

        if (opts.profile) {
            metrics.on_stage_timing(st);
            metrics.record_core(profiler_getcpu());
            if (opts.report_interval_sec > 0 &&
                ns_to_ms(t_det - last_report) >=
                    opts.report_interval_sec * 1000.0) {
                metrics.sample_resource(ns_to_ms(t_det - start) / 1000.0);
                last_report = t_det;
            }
        }
    }
    const double dur = ns_to_ms(monotonic_ns() - start) / 1000.0;
    std::printf("TENNIS_TEST_YOLO model=%s ball_class=%d frames=%llu "
                "detections=%llu inference_errors=%llu duration_sec=%.3f "
                "infer_fps=%.2f\n",
                opts.model.c_str(), cfg.ball_class_id,
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(detections),
                static_cast<unsigned long long>(infer_errors), dur,
                dur > 0 ? static_cast<double>(frames) / dur : 0.0);
    if (opts.profile) {
        ColdStart cold;
        cold.capture_init_ms = ns_to_ms(t_cap1 - t_cap0);
        cold.model_init_ms = ns_to_ms(t_mdl1 - t_mdl0);
        if (t_first_frame) cold.first_frame_wait_ms = ns_to_ms(t_first_frame - t_mdl1);
        if (t_first_detect && t_first_frame)
            cold.first_detection_ms = ns_to_ms(t_first_detect - t_first_frame);
        metrics.set_cold_start(cold);
        metrics.emit_profile_result();
        metrics.emit_pipeline_result();
        metrics.emit_cold_start();
        metrics.emit_core_hist(opts.infer_affinity.empty()
                                   ? "none"
                                   : opts.infer_affinity.c_str());
        metrics.close_csv();
    }
    std::printf("TENNIS_TEST_YOLO_DONE\n");
    cam.stop();
    det.deinit();
    return 0;
}

int run_test_bucket(const Options &opts) {
    Config cfg = make_cfg(opts);
    Camera cam;
    const int64_t t_cap0 = monotonic_ns();
    if (!cam.start(opts.device, opts.width, opts.height, opts.fps)) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }
    const int64_t t_cap1 = monotonic_ns();
    BucketDetector bucket(cfg);

    Metrics metrics;
    metrics.reserve(static_cast<size_t>(opts.duration_sec * opts.fps) + 16);
    if (opts.profile && !opts.profile_csv.empty()) {
        metrics.open_csv(opts.profile_csv.c_str());
    }
    if (!opts.infer_affinity.empty()) {
        apply_and_check_affinity(opts.infer_affinity.c_str());
    }

    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t ts = 0;
    uint64_t frames = 0, bucket_detections = 0;
    int64_t last_report = start;
    int64_t t_first_frame = 0, t_first_detect = 0;
    while (monotonic_ns() < end) {
        if (!cam.poll(lf, ts)) {
            sleep_ns(kIdleSleepNs);
            continue;
        }
        const int64_t pickup = monotonic_ns();
        if (t_first_frame == 0) t_first_frame = pickup;
        StageTiming st;
        st.seq = lf.id;
        st.queue_age_ms = ns_to_ms(pickup - ts);

        image_buffer_t img{};
        const int64_t t_dec0 = monotonic_ns();
        if (frame_to_image(lf, &img) != 0) continue;
        st.decode_ms = ns_to_ms(monotonic_ns() - t_dec0);

        BucketObs b;
        const int64_t t_det0 = monotonic_ns();
        bucket.detect(&img, b);
        const int64_t t_det1 = monotonic_ns();
        if (t_first_detect == 0) t_first_detect = t_det1;
        // No NPU here; record the HSV detect as the detection-compute stage.
        st.run_ms = ns_to_ms(t_det1 - t_det0);
        st.frame_to_detection_ms = ns_to_ms(t_det1 - ts);
        st.detections = b.found ? 1 : 0;
        if (b.found) ++bucket_detections;
        ++frames;
        if (img.virt_addr) std::free(img.virt_addr);

        if (opts.profile) {
            metrics.on_stage_timing(st);
            metrics.record_core(profiler_getcpu());
            if (opts.report_interval_sec > 0 &&
                ns_to_ms(t_det1 - last_report) >=
                    opts.report_interval_sec * 1000.0) {
                metrics.sample_resource(ns_to_ms(t_det1 - start) / 1000.0);
                last_report = t_det1;
            }
        }
    }
    const double dur = ns_to_ms(monotonic_ns() - start) / 1000.0;
    std::printf("TENNIS_TEST_BUCKET frames=%llu bucket_detections=%llu "
                "duration_sec=%.3f fps=%.2f\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(bucket_detections), dur,
                dur > 0 ? static_cast<double>(frames) / dur : 0.0);
    if (opts.profile) {
        ColdStart cold;
        cold.capture_init_ms = ns_to_ms(t_cap1 - t_cap0);
        if (t_first_frame) cold.first_frame_wait_ms = ns_to_ms(t_first_frame - t_cap1);
        if (t_first_detect && t_first_frame)
            cold.first_detection_ms = ns_to_ms(t_first_detect - t_first_frame);
        metrics.set_cold_start(cold);
        metrics.emit_profile_result();
        metrics.emit_pipeline_result();
        metrics.emit_cold_start();
        metrics.emit_core_hist(opts.infer_affinity.empty()
                                   ? "none"
                                   : opts.infer_affinity.c_str());
        metrics.close_csv();
    }
    std::printf("TENNIS_TEST_BUCKET_DONE\n");
    cam.stop();
    return 0;
}

int run_validate(const Options &opts) {
    Config cfg = make_cfg(opts);
    TennisDetector det;
    if (!det.init(opts.model.c_str(), opts.label.c_str(), cfg, opts.core_mask)) {
        std::fprintf(stderr, "rknn_init fail! model=%s\n", opts.model.c_str());
        return 1;
    }
    std::printf("TENNIS_VALIDATE_BEGIN model=%s single_class=%d model_w=%d "
                "model_h=%d min_confidence=%.2f\n",
                opts.model.c_str(), det.single_class() ? 1 : 0,
                det.model_width(), det.model_height(), cfg.conf_thresh);

    FILE *f = std::fopen(opts.validate_list.c_str(), "r");
    if (!f) {
        std::fprintf(stderr, "TENNIS_ERROR cannot open validate-list %s\n",
                     opts.validate_list.c_str());
        det.deinit();
        return 2;
    }

    char line[1024];
    int total = 0, detected = 0;
    double score_sum = 0.0;
    while (std::fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        size_t len = std::strlen(p);
        while (len > 0 &&
               (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
            p[--len] = '\0';
        if (len == 0 || p[0] == '#') continue;

        image_buffer_t img{};
        if (read_image(p, &img) != 0) {
            std::printf("TENNIS_VALIDATE image=%s read_error=1\n", p);
            continue;
        }
        ++total;
        BallObs ball;
        rknn_inference_profile_t prof{};
        int rc = det.detect(&img, ball, &prof);
        if (ball.found) {
            ++detected;
            score_sum += ball.score;
        }
        std::printf("TENNIS_VALIDATE image=%s img_w=%d img_h=%d found=%d "
                    "score=%.3f cx=%.1f cy=%.1f bw=%.1f bh=%.1f area_ratio=%.4f "
                    "run_ms=%.2f rc=%d\n",
                    p, img.width, img.height, ball.found ? 1 : 0, ball.score,
                    ball.cx, ball.cy, ball.w, ball.h, ball.area_ratio,
                    prof.run_ms, rc);
        if (img.virt_addr) std::free(img.virt_addr);
    }
    std::fclose(f);
    std::printf("TENNIS_VALIDATE_RESULT model=%s images=%d detected=%d "
                "mean_score=%.3f\n",
                opts.model.c_str(), total, detected,
                detected > 0 ? score_sum / detected : 0.0);
    std::printf("TENNIS_VALIDATE_DONE\n");
    det.deinit();
    return 0;
}

} // namespace tennis
