// SPDX-License-Identifier: Apache-2.0
//
// Live pipeline + test modes for the board.
//
// Threading: libuvc runs its own capture thread and latches only the newest
// frame; this file's main loop polls that latest-frame slot, runs
// perception (YOLO or HSV, per the FSM's current mode), and the control step.
// Because the actuator backend is non-blocking (TraceBackend), fusing
// perception+control on one thread keeps frame->command latency at pure compute;
// the separate control thread + multi-context NPU workers are deferred (they
// matter once a *blocking* real UART backend lands). See README "Next steps".
#include "live.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "bench/metrics.h"
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

static Config make_cfg(const Options &opts) {
    Config cfg = opts.cfg;
    cfg.frame_w = opts.width;
    cfg.frame_h = opts.height;
    return cfg;
}

int run_live(const Options &opts) {
    Config cfg = make_cfg(opts);

    Camera cam;
    if (!cam.start(opts.device, opts.width, opts.height, opts.fps)) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }

    TennisDetector det;
    if (!det.init(opts.model.c_str(), opts.label.c_str(), cfg, opts.core_mask)) {
        std::fprintf(stderr, "rknn_init fail! model=%s\n", opts.model.c_str());
        cam.stop();
        return 1;
    }

    BucketDetector bucket(cfg);
    TraceMotorBackend motor;
    TraceArmBackend arm;
    Metrics metrics;
    metrics.reserve(static_cast<size_t>(opts.duration_sec * opts.fps) + 16);
    Controller controller(cfg, motor, arm, metrics, opts.log_every);

    std::printf("TENNIS_BENCH_BEGIN mode=live model=%s fps=%d duration_sec=%.3f "
                "core_mask=%s virtual_actuators=%d\n",
                opts.model.c_str(), opts.fps, opts.duration_sec,
                opts.core_mask.c_str(), opts.virtual_actuators ? 1 : 0);

    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t capture_ts = 0;

    while (monotonic_ns() < end) {
        if (!cam.poll(lf, capture_ts)) {
            sleep_ns(kIdleSleepNs);
            continue;
        }

        // Drop frames already older than the staleness bound (0 = never).
        if (cfg.staleness_ms > 0 &&
            ns_to_ms(monotonic_ns() - capture_ts) >
                static_cast<double>(cfg.staleness_ms)) {
            continue;
        }

        image_buffer_t img{};
        if (frame_to_image(lf, &img) != 0) {
            metrics.on_decode_error();
            continue;
        }

        Detection d;
        d.seq = lf.id;
        d.capture_ts_ns = capture_ts;
        d.mode = controller.perception_mode();
        d.valid = true;

        if (d.mode == PerceptionMode::BALL) {
            rknn_inference_profile_t prof{};
            if (det.detect(&img, d.ball, &prof) != 0) {
                metrics.on_inference_error();
            }
        } else {
            bucket.detect(&img, d.bucket);
        }
        d.detect_ts_ns = monotonic_ns();

        if (img.virt_addr) std::free(img.virt_addr);

        controller.process(d);
    }

    const double dur = ns_to_ms(monotonic_ns() - start) / 1000.0;
    const UvcCaptureCounters cc = cam.counters();
    metrics.set_captured(cc.captured);
    metrics.emit_result(dur);
    std::printf("TENNIS_BENCH_DONE\n");

    cam.stop();
    det.deinit();
    return 0;
}

int run_test_uvc(const Options &opts) {
    Camera cam;
    if (!cam.start(opts.device, opts.width, opts.height, opts.fps)) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }
    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t ts = 0;
    uint64_t polled = 0;
    while (monotonic_ns() < end) {
        if (cam.poll(lf, ts)) {
            ++polled;
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
    std::printf("TENNIS_TEST_UVC_DONE\n");
    cam.stop();
    return 0;
}

int run_test_yolo(const Options &opts) {
    Config cfg = make_cfg(opts);
    Camera cam;
    if (!cam.start(opts.device, opts.width, opts.height, opts.fps)) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }
    TennisDetector det;
    if (!det.init(opts.model.c_str(), opts.label.c_str(), cfg, opts.core_mask)) {
        std::fprintf(stderr, "rknn_init fail! model=%s\n", opts.model.c_str());
        cam.stop();
        return 1;
    }
    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t ts = 0;
    uint64_t frames = 0, detections = 0, infer_errors = 0;
    while (monotonic_ns() < end) {
        if (!cam.poll(lf, ts)) {
            sleep_ns(kIdleSleepNs);
            continue;
        }
        image_buffer_t img{};
        if (frame_to_image(lf, &img) != 0) continue;
        BallObs ball;
        rknn_inference_profile_t prof{};
        if (det.detect(&img, ball, &prof) != 0) {
            ++infer_errors;
        } else if (ball.found) {
            ++detections;
        }
        ++frames;
        if (img.virt_addr) std::free(img.virt_addr);
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
    std::printf("TENNIS_TEST_YOLO_DONE\n");
    cam.stop();
    det.deinit();
    return 0;
}

int run_test_bucket(const Options &opts) {
    Config cfg = make_cfg(opts);
    Camera cam;
    if (!cam.start(opts.device, opts.width, opts.height, opts.fps)) {
        std::fprintf(stderr, "uvc_start_streaming failed: device %d\n",
                     opts.device);
        return 1;
    }
    BucketDetector bucket(cfg);
    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    LatestFrame lf;
    int64_t ts = 0;
    uint64_t frames = 0, bucket_detections = 0;
    while (monotonic_ns() < end) {
        if (!cam.poll(lf, ts)) {
            sleep_ns(kIdleSleepNs);
            continue;
        }
        image_buffer_t img{};
        if (frame_to_image(lf, &img) != 0) continue;
        BucketObs b;
        bucket.detect(&img, b);
        if (b.found) ++bucket_detections;
        ++frames;
        if (img.virt_addr) std::free(img.virt_addr);
    }
    const double dur = ns_to_ms(monotonic_ns() - start) / 1000.0;
    std::printf("TENNIS_TEST_BUCKET frames=%llu bucket_detections=%llu "
                "duration_sec=%.3f fps=%.2f\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(bucket_detections), dur,
                dur > 0 ? static_cast<double>(frames) / dur : 0.0);
    std::printf("TENNIS_TEST_BUCKET_DONE\n");
    cam.stop();
    return 0;
}

} // namespace tennis
