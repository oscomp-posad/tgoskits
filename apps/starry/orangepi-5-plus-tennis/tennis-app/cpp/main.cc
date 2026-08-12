// SPDX-License-Identifier: Apache-2.0
//
// Entry point for the StarryOS tennis-ball pickup demo/benchmark.
//
//   tennis_app --mode live      --model model/tennis.rknn --label model/labels.txt \
//              --device 0 --width 640 --height 480 --fps 30 --duration-sec 60 --virtual-actuators
//   tennis_app --mode test-uvc   --device 0
//   tennis_app --mode test-yolo  --model model/tennis.rknn --device 0
//   tennis_app --mode test-bucket --device 0
//   tennis_app --mode dry-run    --duration-sec 10 --virtual-actuators
//
// `dry-run` needs no camera, model, or board: it drives the full state machine
// from a deterministic synthetic scene and emits the mandatory TENNIS_BENCH_RESULT
// line, so the benchmark output is reproducible anywhere (and on the host build,
// `cmake -DTENNIS_HOST_DRYRUN=ON`, it is the only available mode).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // sched_* in profiling.h (Linux/StarryOS; stubbed elsewhere)
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "actuator/arm_backend.h"
#include "actuator/motor_backend.h"
#include "app_options.h"
#include "bench/metrics.h"
#include "controller.h"
#include "profiling.h"
#include "state_machine.h"
#include "time_utils.h"
#include "types.h"

#ifndef TENNIS_HOST_DRYRUN
#include "live.h" // run_live / run_test_uvc / run_test_yolo / run_test_bucket
#endif

namespace tennis {

// Deterministic synthetic scene for dry-run. It advances a ball-approach and a
// bucket-approach driven by the FSM's current state, so a full pickup cycle
// (chase -> grab -> find -> approach -> deposit -> repeat) plays out without any
// hardware. Detection latency is real monotonic time through this cheap path,
// so the dry-run benchmark reflects control-loop plumbing, not real decode/infer.
class SyntheticScene {
public:
    explicit SyntheticScene(const Config &cfg)
        : cfg_(cfg), half_w_(cfg.frame_w / 2) {}

    Detection next(PerceptionMode mode, GameState state, uint64_t seq,
                   int64_t capture_ts) {
        if (state != prev_state_) {
            on_transition(state);
            prev_state_ = state;
        }

        Detection d;
        d.seq = seq;
        d.capture_ts_ns = capture_ts;
        d.mode = mode;
        d.valid = true;

        if (mode == PerceptionMode::BALL) {
            if (state == GameState::CHASE_BALL) ball_p_ += 0.02f;
            const float p = ball_p_ > 1.f ? 1.f : ball_p_;
            // Brief occlusion to exercise the lost/search path.
            const bool occluded = ball_p_ > 0.40f && ball_p_ < 0.46f;
            if (!occluded) {
                d.ball.found = true;
                d.ball.area_ratio = 0.03f + 0.29f * p; // crosses area_stop near the end
                const float off = 160.f - 70.f * p;    // converges to stop_center_offset
                d.ball.cx = static_cast<float>(half_w_) + off;
                d.ball.cy = static_cast<float>(cfg_.frame_h) / 2.f;
                const float px =
                    d.ball.area_ratio * cfg_.frame_w * cfg_.frame_h;
                d.ball.w = d.ball.h =
                    px > 0.f ? static_cast<float>(std::sqrt(px)) : 0.f;
                d.ball.score = 0.92f;
            }
        } else {
            // Bucket: a couple of "not found" frames in FIND to exercise search,
            // then a converging approach.
            if (state == GameState::FIND_BUCKET && bucket_seen_ < 2) {
                ++bucket_seen_;
            } else {
                bucket_p_ += 0.05f;
                const float p = bucket_p_ > 1.f ? 1.f : bucket_p_;
                d.bucket.found = true;
                d.bucket.area_ratio = 0.20f + 0.75f * p; // crosses deposit (0.90)
                d.bucket.cx = static_cast<float>(half_w_) + (40.f - 40.f * p);
                d.bucket.cy = static_cast<float>(cfg_.frame_h) / 2.f;
                d.bucket.w = d.bucket.h = 120;
                d.bucket.x = static_cast<int>(d.bucket.cx) - 60;
                d.bucket.y = static_cast<int>(d.bucket.cy) - 60;
            }
        }

        d.detect_ts_ns = monotonic_ns();
        return d;
    }

private:
    void on_transition(GameState s) {
        switch (s) {
        case GameState::CHASE_BALL:
            ball_p_ = 0.f;
            bucket_p_ = 0.f;
            bucket_seen_ = 0;
            break;
        case GameState::FIND_BUCKET:
            bucket_p_ = 0.f;
            bucket_seen_ = 0;
            break;
        default: break;
        }
    }

    Config cfg_;
    int half_w_;
    GameState prev_state_ = GameState::CHASE_BALL;
    float ball_p_ = 0.f;
    float bucket_p_ = 0.f;
    int bucket_seen_ = 0;
};

static int run_dry_run(const Options &opts) {
    Config cfg = opts.cfg;
    cfg.frame_w = opts.width;
    cfg.frame_h = opts.height;

    const int fps = opts.fps > 0 ? opts.fps : 30;
    const int64_t frame_period_ns = 1000000000LL / fps;
    const size_t expected = static_cast<size_t>(opts.duration_sec * fps) + 16;

    const int64_t t_proc_start = monotonic_ns();
    TraceMotorBackend motor;
    TraceArmBackend arm;
    Metrics metrics;
    metrics.reserve(expected);
    if (opts.profile && !opts.profile_csv.empty()) {
        metrics.open_csv(opts.profile_csv.c_str());
    }
    Controller controller(cfg, motor, arm, metrics, opts.log_every);
    SyntheticScene scene(cfg);

    std::printf("TENNIS_BENCH_BEGIN mode=dry-run fps=%d duration_sec=%.3f "
                "virtual_actuators=%d profile=%d affinity=%s\n",
                fps, opts.duration_sec, opts.virtual_actuators ? 1 : 0,
                opts.profile ? 1 : 0,
                opts.infer_affinity.empty() ? "none"
                                            : opts.infer_affinity.c_str());

    if (!opts.infer_affinity.empty()) {
        apply_and_check_affinity(opts.infer_affinity.c_str());
    }

    const int64_t start = monotonic_ns();
    const int64_t end = start + static_cast<int64_t>(opts.duration_sec * 1e9);
    uint64_t seq = 0;
    int64_t next_frame = start;
    int64_t last_report = start;
    int64_t t_first_cmd = 0;

    while (monotonic_ns() < end) {
        const int64_t capture_ts = monotonic_ns();
        metrics.on_capture();
        Detection det = scene.next(controller.perception_mode(),
                                   controller.state(), seq, capture_ts);
        const int64_t t_ctrl0 = monotonic_ns();
        controller.process(det);
        const int64_t t_ctrl1 = monotonic_ns();
        if (t_first_cmd == 0) t_first_cmd = t_ctrl1;
        ++seq;

        if (opts.profile) {
            StageTiming st;
            st.seq = det.seq;
            st.queue_age_ms = ns_to_ms(t_ctrl0 - capture_ts);
            st.control_ms = ns_to_ms(t_ctrl1 - t_ctrl0);
            st.frame_to_detection_ms = ns_to_ms(det.detect_ts_ns - capture_ts);
            st.frame_to_command_ms = ns_to_ms(t_ctrl1 - capture_ts);
            metrics.on_stage_timing(st);
            metrics.record_core(profiler_getcpu());
            if (opts.report_interval_sec > 0 &&
                ns_to_ms(t_ctrl1 - last_report) >=
                    opts.report_interval_sec * 1000.0) {
                metrics.sample_resource(ns_to_ms(t_ctrl1 - start) / 1000.0);
                last_report = t_ctrl1;
            }
        }

        next_frame += frame_period_ns;
        const int64_t sleep = next_frame - monotonic_ns();
        if (sleep > 0) sleep_ns(sleep);
    }

    const double dur = ns_to_ms(monotonic_ns() - start) / 1000.0;
    if (opts.profile) {
        ColdStart cold;
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
    return 0;
}

static void usage(const char *prog) {
    std::fprintf(
        stderr,
        "usage: %s --mode <live|test-uvc|test-yolo|test-bucket|validate|dry-run> [opts]\n"
        "  --validate-list <file>   (validate mode) fixed-image accuracy check; one image path per line\n"
        "  --model <path>           .rknn model (default model/tennis.rknn)\n"
        "  --label <path>           labels file (default model/labels.txt)\n"
        "  --device <n>             UVC device index (default 0)\n"
        "  --width/--height <n>     capture size (default 640x480)\n"
        "  --fps <n>                capture fps (default 30)\n"
        "  --duration-sec <f>       run duration (default 60)\n"
        "  --virtual-actuators      use the Trace motor/arm backends (default)\n"
        "  --ball-class <n>         class id of the ball (0 tennis model; 32 COCO)\n"
        "  --min-confidence <0-100> detection confidence threshold (default 50)\n"
        "  --log-every <n>          emit per-frame lines every Nth frame\n"
        "  --staleness-ms <n>       drop detections older than n ms (0 = never)\n"
        "  --core-mask <m>          NPU core mask for live mode (default all)\n"
        "  --profile                collect per-stage timing + emit PROFILE/"
        "PIPELINE/COLD_START/RES lines\n"
        "  --report-interval-sec <n> TENNIS_RES cadence (default 5)\n"
        "  --profile-csv <path>     write one CSV row per processed frame\n"
        "  --infer-affinity <list>  pin inference thread to CPUs, e.g. 4-7 "
        "(RK3588 A76 big cores)\n",
        prog);
}

static bool arg_val(int argc, char **argv, int &i, const char *flag,
                    std::string &out) {
    if (std::strcmp(argv[i], flag) == 0) {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "TENNIS_ERROR missing value for %s\n", flag);
            std::exit(2);
        }
        out = argv[++i];
        return true;
    }
    return false;
}

static int parse_options(int argc, char **argv, Options &o) {
    std::string v;
    for (int i = 1; i < argc; ++i) {
        if (arg_val(argc, argv, i, "--mode", o.mode)) continue;
        if (arg_val(argc, argv, i, "--model", o.model)) continue;
        if (arg_val(argc, argv, i, "--label", o.label)) continue;
        if (arg_val(argc, argv, i, "--core-mask", o.core_mask)) continue;
        if (arg_val(argc, argv, i, "--profile-csv", o.profile_csv)) continue;
        if (arg_val(argc, argv, i, "--infer-affinity", o.infer_affinity)) continue;
        if (arg_val(argc, argv, i, "--validate-list", o.validate_list)) continue;
        if (arg_val(argc, argv, i, "--report-interval-sec", v)) { o.report_interval_sec = std::atoi(v.c_str()); continue; }
        if (std::strcmp(argv[i], "--profile") == 0) { o.profile = true; continue; }
        if (arg_val(argc, argv, i, "--device", v)) { o.device = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--width", v)) { o.width = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--height", v)) { o.height = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--fps", v)) { o.fps = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--duration-sec", v)) { o.duration_sec = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--ball-class", v)) { o.cfg.ball_class_id = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--log-every", v)) { o.log_every = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--staleness-ms", v)) { o.cfg.staleness_ms = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--min-confidence", v)) {
            int c = std::atoi(v.c_str());
            if (c < 0) c = 0;
            if (c > 100) c = 100;
            o.cfg.conf_thresh = static_cast<float>(c) / 100.f;
            continue;
        }
        if (std::strcmp(argv[i], "--virtual-actuators") == 0) { o.virtual_actuators = true; continue; }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            std::exit(0);
        }
        std::fprintf(stderr, "TENNIS_ERROR unknown argument: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }
    o.cfg.frame_w = o.width;
    o.cfg.frame_h = o.height;
    return 0;
}

} // namespace tennis

int main(int argc, char **argv) {
    using namespace tennis;
    Options opts;
    if (int rc = parse_options(argc, argv, opts)) return rc;

    if (opts.mode == "dry-run") return run_dry_run(opts);

#ifndef TENNIS_HOST_DRYRUN
    if (opts.mode == "live") return run_live(opts);
    if (opts.mode == "test-uvc") return run_test_uvc(opts);
    if (opts.mode == "test-yolo") return run_test_yolo(opts);
    if (opts.mode == "test-bucket") return run_test_bucket(opts);
    if (opts.mode == "validate") return run_validate(opts);
    std::fprintf(stderr, "TENNIS_ERROR unknown mode: %s\n", opts.mode.c_str());
    return 2;
#else
    std::fprintf(stderr,
                 "TENNIS_ERROR mode '%s' requires the board build (RKNN/UVC); "
                 "only --mode dry-run is available in the host build\n",
                 opts.mode.c_str());
    return 2;
#endif
}
