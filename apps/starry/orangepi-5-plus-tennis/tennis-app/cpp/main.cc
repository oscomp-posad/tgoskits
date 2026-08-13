// SPDX-License-Identifier: Apache-2.0
//
// Entry point for the StarryOS tennis-ball pickup demo/benchmark.
//
//   tennis_app --mode live --model model/tennis.rknn --label model/labels.txt
//              --device 0 --width 640 --height 480 --fps 30 --duration-sec 60
//              --virtual-actuators
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "actuator/arm_backend.h"
#include "actuator/actuator_factory.h"
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
    Actuators actuators;
    if (!make_actuators(opts, actuators)) return 1;
    Metrics metrics;
    metrics.reserve(expected);
    if (opts.profile && !opts.profile_csv.empty()) {
        metrics.open_csv(opts.profile_csv.c_str());
    }
    Controller controller(cfg, *actuators.motor, *actuators.arm, metrics,
                          opts.log_every);
    SyntheticScene scene(cfg);

    std::printf("TENNIS_BENCH_BEGIN mode=dry-run fps=%d duration_sec=%.3f "
                "virtual_actuators=%d motor_backend=%s arm_backend=%s "
                "profile=%d affinity=%s\n",
                fps, opts.duration_sec, uses_virtual_actuators(opts) ? 1 : 0,
                opts.motor_backend.c_str(), opts.arm_backend.c_str(),
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
    bool actuator_failed = false;

    while (monotonic_ns() < end) {
        const int64_t capture_ts = monotonic_ns();
        metrics.on_capture();
        Detection det = scene.next(controller.perception_mode(),
                                   controller.state(), seq, capture_ts);
        const int64_t t_ctrl0 = monotonic_ns();
        if (!controller.process(det)) {
            actuator_failed = true;
            break;
        }
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
    if (actuator_failed) (void)actuators.motor->standby();

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
    return actuator_failed ? 1 : 0;
}

static void usage(const char *prog) {
    std::fprintf(
        stderr,
        "usage: %s --mode <live|test-uvc|test-yolo|test-bucket|validate|dry-run> [opts]\n"
        "  --config <path>         load key=value options; CLI options override it\n"
        "  --validate-list <file>   (validate mode) fixed-image accuracy check; one image path per line\n"
        "  --model <path>           .rknn model (default model/tennis.rknn)\n"
        "  --label <path>           labels file (default model/labels.txt)\n"
        "  --device <n>             UVC device index (default 0)\n"
        "  --width/--height <n>     capture size (default 640x480)\n"
        "  --fps <n>                capture fps (default 30)\n"
        "  --duration-sec <f>       run duration (default 60)\n"
        "  --motor-backend <kind>   virtual|pwm|uart (default virtual)\n"
        "  --motor-device <spec>    PWM chip list or UART device (UART default /dev/ttyS6)\n"
        "  --arm-backend <kind>     virtual|uart (default virtual)\n"
        "  --arm-device <path>      arm UART device (default /dev/ttyS3)\n"
        "  --motor-min-speed <n>    translation floor before steering bias (default 20)\n"
        "  --area-far <f>           ball area ratio below which far speed is used\n"
        "  --area-stop <f>          ball area ratio that stops the approach\n"
        "  --area-reverse <f>       ball area ratio that triggers reverse\n"
        "  --stop-center-offset <n> gripper target offset from image center\n"
        "  --stop-center-zone <n>   final horizontal tolerance in pixels\n"
        "  --stop-confirm-cnt <n>   consecutive aligned frames before grab\n"
        "  --chase-far-speed <n>    far-distance ball approach speed\n"
        "  --chase-forward-speed <n> near-distance ball approach speed\n"
        "  --chase-turn-k <f>       proportional ball steering gain\n"
        "  --chase-max-bias <n>    maximum ball steering wheel bias\n"
        "  --chase-close-pivot-speed <n> fixed close-ball alignment speed\n"
        "  --reverse-speed <n>      too-close reverse speed\n"
        "  --deposit-reverse-speed <n> speed after releasing into bucket\n"
        "  --deposit-reverse-ms <n> reverse duration after release (0 disables)\n"
        "  --search-pivot-speed <n> ball search rotation speed\n"
        "  --search-reverse-turns <f> estimated turns before reversing search\n"
        "  --odometry-enabled <bool> enable RPM odometry return guidance\n"
        "  --odometry-sample-ms <n> RPM sampling interval\n"
        "  --return-timeout-ms <n>  odometry return timeout\n"
        "  --return-stop-radius <f> stop radius before visual bucket search\n"
        "  --bucket-min-area <n>    minimum connected red pixels for a bucket\n"
        "  --bucket-area-brake <f>  bucket area ratio that selects near speed\n"
        "  --bucket-area-deposit <f> bucket area ratio that triggers deposit\n"
        "  --bucket-approach-speed <n> far-distance bucket approach speed\n"
        "  --bucket-brake-speed <n> near-distance bucket approach speed\n"
        "  --bucket-search-speed <n> fixed in-place bucket search speed\n"
        "  --camera-warmup-frames <n> consecutive startup frames (default 3)\n"
        "  --camera-warmup-timeout-ms <n> startup deadline (default 3000)\n"
        "  --camera-watchdog-ms <n> no-frame stop timeout (default 2000)\n"
        "  --virtual-actuators      select both virtual backends (compatibility)\n"
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

static std::string trim(const std::string &value) {
    const char *space = " \t\r\n";
    const size_t first = value.find_first_not_of(space);
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(space);
    return value.substr(first, last - first + 1);
}

static bool parse_config_bool(const std::string &value, bool &result) {
    if (value == "1" || value == "true" || value == "yes" ||
        value == "on") {
        result = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" ||
        value == "off") {
        result = false;
        return true;
    }
    return false;
}

static bool apply_config_value(Options &o, std::string key,
                               const std::string &value) {
    if (key.rfind("--", 0) == 0) key.erase(0, 2);
    std::replace(key.begin(), key.end(), '_', '-');

    if (key == "mode") o.mode = value;
    else if (key == "model") o.model = value;
    else if (key == "label") o.label = value;
    else if (key == "core-mask") o.core_mask = value;
    else if (key == "motor-backend") o.motor_backend = value;
    else if (key == "motor-device") o.motor_device = value;
    else if (key == "arm-backend") o.arm_backend = value;
    else if (key == "arm-device") o.arm_device = value;
    else if (key == "profile-csv") o.profile_csv = value;
    else if (key == "infer-affinity") o.infer_affinity = value;
    else if (key == "validate-list") o.validate_list = value;
    else if (key == "report-interval-sec")
        o.report_interval_sec = std::atoi(value.c_str());
    else if (key == "device") o.device = std::atoi(value.c_str());
    else if (key == "width") o.width = std::atoi(value.c_str());
    else if (key == "height") o.height = std::atoi(value.c_str());
    else if (key == "fps") o.fps = std::atoi(value.c_str());
    else if (key == "duration-sec")
        o.duration_sec = std::atof(value.c_str());
    else if (key == "ball-class")
        o.cfg.ball_class_id = std::atoi(value.c_str());
    else if (key == "log-every") o.log_every = std::atoi(value.c_str());
    else if (key == "staleness-ms")
        o.cfg.staleness_ms = std::atoi(value.c_str());
    else if (key == "motor-min-speed")
        o.cfg.motor_min_speed = std::atoi(value.c_str());
    else if (key == "area-far")
        o.cfg.area_far = std::atof(value.c_str());
    else if (key == "area-stop")
        o.cfg.area_stop = std::atof(value.c_str());
    else if (key == "area-reverse")
        o.cfg.area_reverse = std::atof(value.c_str());
    else if (key == "stop-center-offset")
        o.cfg.stop_center_offset = std::atoi(value.c_str());
    else if (key == "stop-center-zone")
        o.cfg.stop_center_zone = std::atoi(value.c_str());
    else if (key == "stop-confirm-cnt")
        o.cfg.stop_confirm_cnt = std::atoi(value.c_str());
    else if (key == "chase-far-speed")
        o.cfg.chase_far_spd = std::atoi(value.c_str());
    else if (key == "chase-forward-speed")
        o.cfg.chase_forward_spd = std::atoi(value.c_str());
    else if (key == "chase-turn-k")
        o.cfg.chase_turn_k = std::atof(value.c_str());
    else if (key == "chase-max-bias")
        o.cfg.chase_max_bias = std::atoi(value.c_str());
    else if (key == "chase-close-pivot-speed")
        o.cfg.chase_close_pivot_spd = std::atoi(value.c_str());
    else if (key == "reverse-speed")
        o.cfg.reverse_speed = std::atoi(value.c_str());
    else if (key == "deposit-reverse-speed")
        o.cfg.deposit_reverse_speed = std::atoi(value.c_str());
    else if (key == "deposit-reverse-ms")
        o.cfg.deposit_reverse_ms = std::atoi(value.c_str());
    else if (key == "search-pivot-speed")
        o.cfg.search_pivot_spd = std::atoi(value.c_str());
    else if (key == "search-reverse-turns")
        o.cfg.search_reverse_turns = std::atof(value.c_str());
    else if (key == "odometry-enabled") {
        if (!parse_config_bool(value, o.cfg.odometry_enabled)) return false;
    } else if (key == "odometry-wheel-radius-m")
        o.cfg.odometry_wheel_radius_m = std::atof(value.c_str());
    else if (key == "odometry-wheel-base-m")
        o.cfg.odometry_wheel_base_m = std::atof(value.c_str());
    else if (key == "odometry-sample-ms")
        o.cfg.odometry_sample_ms = std::atoi(value.c_str());
    else if (key == "odometry-stale-ms")
        o.cfg.odometry_stale_ms = std::atoi(value.c_str());
    else if (key == "odometry-max-gap-ms")
        o.cfg.odometry_max_gap_ms = std::atoi(value.c_str());
    else if (key == "odometry-max-rpm")
        o.cfg.odometry_max_rpm = std::atoi(value.c_str());
    else if (key == "return-heading-tolerance-deg")
        o.cfg.return_heading_tolerance_deg = std::atof(value.c_str());
    else if (key == "return-stop-radius")
        o.cfg.return_stop_radius_m = std::atof(value.c_str());
    else if (key == "return-max-distance")
        o.cfg.return_max_distance_m = std::atof(value.c_str());
    else if (key == "return-timeout-ms")
        o.cfg.return_timeout_ms = std::atoi(value.c_str());
    else if (key == "return-pivot-speed")
        o.cfg.return_pivot_spd = std::atoi(value.c_str());
    else if (key == "return-forward-speed")
        o.cfg.return_forward_spd = std::atoi(value.c_str());
    else if (key == "bucket-min-area")
        o.cfg.bucket_min_area = std::atoi(value.c_str());
    else if (key == "bucket-area-brake")
        o.cfg.bucket_area_brake = std::atof(value.c_str());
    else if (key == "bucket-area-deposit")
        o.cfg.bucket_area_deposit = std::atof(value.c_str());
    else if (key == "bucket-approach-speed")
        o.cfg.bucket_approach_spd = std::atoi(value.c_str());
    else if (key == "bucket-brake-speed")
        o.cfg.bucket_brake_spd = std::atoi(value.c_str());
    else if (key == "bucket-search-speed")
        o.cfg.bucket_search_spd = std::atoi(value.c_str());
    else if (key == "camera-warmup-frames")
        o.camera_warmup_frames = std::atoi(value.c_str());
    else if (key == "camera-warmup-timeout-ms")
        o.camera_warmup_timeout_ms = std::atoi(value.c_str());
    else if (key == "camera-watchdog-ms")
        o.camera_watchdog_ms = std::atoi(value.c_str());
    else if (key == "min-confidence") {
        int confidence = std::atoi(value.c_str());
        confidence = std::max(0, std::min(confidence, 100));
        o.cfg.conf_thresh = static_cast<float>(confidence) / 100.0f;
    } else if (key == "profile") {
        if (!parse_config_bool(value, o.profile)) return false;
    } else if (key == "virtual-actuators") {
        bool enabled = false;
        if (!parse_config_bool(value, enabled)) return false;
        if (enabled) {
            o.motor_backend = "virtual";
            o.arm_backend = "virtual";
        }
    } else {
        return false;
    }
    return true;
}

static int load_config_file(const std::string &path, Options &o) {
    std::ifstream input(path);
    if (!input) {
        std::fprintf(stderr, "TENNIS_ERROR cannot open config: %s\n",
                     path.c_str());
        return 2;
    }

    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            std::fprintf(stderr,
                         "TENNIS_ERROR config %s:%d must use key=value\n",
                         path.c_str(), line_number);
            return 2;
        }
        std::string key = trim(line.substr(0, separator));
        std::string value = trim(line.substr(separator + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (key.empty() || !apply_config_value(o, key, value)) {
            std::fprintf(stderr,
                         "TENNIS_ERROR invalid config option %s:%d: %s\n",
                         path.c_str(), line_number, key.c_str());
            return 2;
        }
    }
    std::printf("TENNIS_CONFIG loaded=%s\n", path.c_str());
    return 0;
}

static int parse_options(int argc, char **argv, Options &o) {
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") != 0) continue;
        if (i + 1 >= argc) {
            std::fprintf(stderr, "TENNIS_ERROR missing value for --config\n");
            return 2;
        }
        if (!config_path.empty()) {
            std::fprintf(stderr, "TENNIS_ERROR --config may be specified once\n");
            return 2;
        }
        config_path = argv[++i];
    }
    if (!config_path.empty()) {
        if (int rc = load_config_file(config_path, o)) return rc;
    }

    std::string v;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) {
            ++i;
            continue;
        }
        if (arg_val(argc, argv, i, "--mode", o.mode)) continue;
        if (arg_val(argc, argv, i, "--model", o.model)) continue;
        if (arg_val(argc, argv, i, "--label", o.label)) continue;
        if (arg_val(argc, argv, i, "--core-mask", o.core_mask)) continue;
        if (arg_val(argc, argv, i, "--motor-backend", o.motor_backend)) continue;
        if (arg_val(argc, argv, i, "--motor-device", o.motor_device)) continue;
        if (arg_val(argc, argv, i, "--arm-backend", o.arm_backend)) continue;
        if (arg_val(argc, argv, i, "--arm-device", o.arm_device)) continue;
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
        if (arg_val(argc, argv, i, "--motor-min-speed", v)) { o.cfg.motor_min_speed = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--area-far", v)) { o.cfg.area_far = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--area-stop", v)) { o.cfg.area_stop = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--area-reverse", v)) { o.cfg.area_reverse = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--stop-center-offset", v)) { o.cfg.stop_center_offset = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--stop-center-zone", v)) { o.cfg.stop_center_zone = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--stop-confirm-cnt", v)) { o.cfg.stop_confirm_cnt = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--chase-far-speed", v)) { o.cfg.chase_far_spd = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--chase-forward-speed", v)) { o.cfg.chase_forward_spd = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--chase-turn-k", v)) { o.cfg.chase_turn_k = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--chase-max-bias", v)) { o.cfg.chase_max_bias = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--chase-close-pivot-speed", v)) { o.cfg.chase_close_pivot_spd = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--reverse-speed", v)) { o.cfg.reverse_speed = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--deposit-reverse-speed", v)) { o.cfg.deposit_reverse_speed = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--deposit-reverse-ms", v)) { o.cfg.deposit_reverse_ms = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--search-pivot-speed", v)) { o.cfg.search_pivot_spd = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--search-reverse-turns", v)) { o.cfg.search_reverse_turns = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--odometry-enabled", v)) {
            if (!parse_config_bool(v, o.cfg.odometry_enabled)) {
                std::fprintf(stderr, "TENNIS_ERROR invalid --odometry-enabled\n");
                return 2;
            }
            continue;
        }
        if (arg_val(argc, argv, i, "--odometry-sample-ms", v)) { o.cfg.odometry_sample_ms = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--return-timeout-ms", v)) { o.cfg.return_timeout_ms = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--return-stop-radius", v)) { o.cfg.return_stop_radius_m = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--bucket-min-area", v)) { o.cfg.bucket_min_area = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--bucket-area-brake", v)) { o.cfg.bucket_area_brake = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--bucket-area-deposit", v)) { o.cfg.bucket_area_deposit = std::atof(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--bucket-approach-speed", v)) { o.cfg.bucket_approach_spd = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--bucket-brake-speed", v)) { o.cfg.bucket_brake_spd = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--bucket-search-speed", v)) { o.cfg.bucket_search_spd = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--camera-warmup-frames", v)) { o.camera_warmup_frames = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--camera-warmup-timeout-ms", v)) { o.camera_warmup_timeout_ms = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--camera-watchdog-ms", v)) { o.camera_watchdog_ms = std::atoi(v.c_str()); continue; }
        if (arg_val(argc, argv, i, "--min-confidence", v)) {
            int c = std::atoi(v.c_str());
            if (c < 0) c = 0;
            if (c > 100) c = 100;
            o.cfg.conf_thresh = static_cast<float>(c) / 100.f;
            continue;
        }
        if (std::strcmp(argv[i], "--virtual-actuators") == 0) {
            o.motor_backend = "virtual";
            o.arm_backend = "virtual";
            continue;
        }
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
    if (o.cfg.motor_min_speed < 1 || o.cfg.motor_min_speed > 100) {
        std::fprintf(stderr,
                     "TENNIS_ERROR --motor-min-speed must be in [1,100]\n");
        return 2;
    }
    if (o.cfg.area_far <= 0.0f || o.cfg.area_far >= o.cfg.area_stop ||
        o.cfg.area_stop >= o.cfg.area_reverse ||
        o.cfg.area_reverse > 1.0f ||
        o.cfg.stop_center_zone < 0 || o.cfg.stop_confirm_cnt < 1 ||
        o.cfg.chase_far_spd < o.cfg.chase_forward_spd ||
        o.cfg.chase_far_spd > 100 ||
        o.cfg.chase_forward_spd < o.cfg.motor_min_speed ||
        o.cfg.chase_forward_spd > 100 ||
        !std::isfinite(o.cfg.chase_turn_k) || o.cfg.chase_turn_k <= 0.0f ||
        o.cfg.chase_max_bias < 1 || o.cfg.chase_max_bias > 50 ||
        o.cfg.chase_close_pivot_spd < o.cfg.motor_min_speed ||
        o.cfg.chase_close_pivot_spd > 100 ||
        o.cfg.reverse_speed < o.cfg.motor_min_speed ||
        o.cfg.reverse_speed > 100 ||
        o.cfg.deposit_reverse_speed < o.cfg.motor_min_speed ||
        o.cfg.deposit_reverse_speed > 100 ||
        o.cfg.deposit_reverse_ms < 0 ||
        o.cfg.search_pivot_spd < o.cfg.motor_min_speed ||
        o.cfg.search_pivot_spd > 100 ||
        !std::isfinite(o.cfg.search_reverse_turns) ||
        o.cfg.search_reverse_turns <= 0.0) {
        std::fprintf(stderr,
                     "TENNIS_ERROR invalid ball pursuit values\n");
        return 2;
    }
    if (o.cfg.odometry_wheel_radius_m <= 0.0 ||
        o.cfg.odometry_wheel_base_m <= 0.0 || o.cfg.odometry_sample_ms <= 0 ||
        o.cfg.odometry_stale_ms < o.cfg.odometry_sample_ms ||
        o.cfg.odometry_max_gap_ms < o.cfg.odometry_sample_ms ||
        o.cfg.odometry_max_rpm <= 0 || o.cfg.return_heading_tolerance_deg <= 0.0 ||
        o.cfg.return_heading_tolerance_deg >= 180.0 ||
        o.cfg.return_stop_radius_m <= 0.0 ||
        o.cfg.return_max_distance_m <= o.cfg.return_stop_radius_m ||
        o.cfg.return_timeout_ms <= 0 ||
        o.cfg.return_pivot_spd < o.cfg.motor_min_speed ||
        o.cfg.return_pivot_spd > 100 ||
        o.cfg.return_forward_spd < o.cfg.motor_min_speed ||
        o.cfg.return_forward_spd > 100) {
        std::fprintf(stderr, "TENNIS_ERROR invalid odometry return values\n");
        return 2;
    }
    const int64_t frame_pixels =
        static_cast<int64_t>(o.cfg.frame_w) * o.cfg.frame_h;
    if (o.cfg.bucket_min_area <= 0 || o.cfg.bucket_min_area > frame_pixels ||
        !std::isfinite(o.cfg.bucket_area_brake) ||
        !std::isfinite(o.cfg.bucket_area_deposit) ||
        o.cfg.bucket_area_brake <= 0.0f ||
        o.cfg.bucket_area_brake >= o.cfg.bucket_area_deposit ||
        o.cfg.bucket_area_deposit > 1.0f ||
        o.cfg.bucket_approach_spd < o.cfg.bucket_brake_spd ||
        o.cfg.bucket_approach_spd > 100 ||
        o.cfg.bucket_brake_spd < o.cfg.motor_min_speed ||
        o.cfg.bucket_brake_spd > 100 ||
        o.cfg.bucket_search_spd < o.cfg.motor_min_speed ||
        o.cfg.bucket_search_spd > 100) {
        std::fprintf(stderr, "TENNIS_ERROR invalid bucket detection values\n");
        return 2;
    }
    if (o.camera_warmup_frames < 0 || o.camera_warmup_timeout_ms <= 0 ||
        o.camera_watchdog_ms <= 0) {
        std::fprintf(stderr,
                     "TENNIS_ERROR camera warmup/watchdog values are invalid\n");
        return 2;
    }
    if (o.motor_backend != "virtual" && o.motor_backend != "pwm" &&
        o.motor_backend != "uart") {
        std::fprintf(stderr, "TENNIS_ERROR unsupported motor backend: %s\n",
                     o.motor_backend.c_str());
        return 2;
    }
    if (o.arm_backend != "virtual" && o.arm_backend != "uart") {
        std::fprintf(stderr, "TENNIS_ERROR unsupported arm backend: %s\n",
                     o.arm_backend.c_str());
        return 2;
    }
    if (o.mode == "dry-run" &&
        (o.motor_backend != "virtual" || o.arm_backend != "virtual")) {
        std::fprintf(stderr,
                     "TENNIS_ERROR dry-run requires virtual actuators\n");
        return 2;
    }
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
