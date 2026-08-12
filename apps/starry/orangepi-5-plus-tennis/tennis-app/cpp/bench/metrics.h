// SPDX-License-Identifier: Apache-2.0
//
// Benchmark accumulator. The benchmark line is the primary deliverable, so the
// hot path must stay allocation-free: per-frame latency samples are pushed into
// vectors reserved up front, running totals are plain integers, and percentiles
// are computed once (sort-on-a-copy) at the end of the run.
//
// Beyond the headline TENNIS_BENCH_RESULT, this accumulator captures the same
// depth of profiling as the sibling uvc-rknn bench: per-stage latencies with
// full tail percentiles (TENNIS_BENCH_PROFILE_RESULT), pipeline timing incl.
// frame queue-age (TENNIS_PIPELINE_RESULT), a cold-start breakdown
// (TENNIS_COLD_START), a resource time-series (TENNIS_RES), a per-frame CSV, and
// an inference-thread core-residency histogram (TENNIS_CORE_HIST). To keep this
// header buildable in BOTH the host dry-run and the board build, it never
// depends on the board-only RKNN headers: the live loop extracts the per-stage
// values from rknn_inference_profile_t and passes them in as plain doubles.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace tennis {

// p in [0,1]; linear-interpolated percentile over a copy of `samples`
// (`samples` is taken by value so the caller's vector is left untouched).
double percentile_ms(std::vector<double> samples, double p);

// Resident set size in KiB (VmRSS on Linux/StarryOS; getrusage fallback
// elsewhere). 0 if unavailable.
size_t read_rss_kb();

constexpr int kMaxCpus = 64;

// Per-frame stage latencies (ms). The RKNN fields are 0 for non-RKNN frames
// (bucket/HSV mode, or the host dry-run); filled by the live loop from the
// rknn_inference_profile_t so this header stays independent of yolov8.h.
struct StageTiming {
    uint64_t seq = 0;
    double queue_age_ms = 0;       // frame age in the latest-frame slot at pickup
    double decode_ms = 0;          // YUYV/MJPEG -> RGB
    double letterbox_ms = 0;
    double inputs_set_ms = 0;
    double run_ms = 0;             // NPU run (wall)
    double rknn_perf_run_ms = 0;   // NPU run (driver PERF_RUN query)
    double outputs_get_ms = 0;
    double postprocess_ms = 0;
    double control_ms = 0;         // state machine + actuator dispatch
    double frame_to_detection_ms = 0;
    double frame_to_command_ms = 0;
    int detections = 0;
};

// Cold-start breakdown: process start -> first command ready.
struct ColdStart {
    double capture_init_ms = 0;          // Camera::start
    double model_init_ms = 0;            // TennisDetector::init (rknn_init)
    double first_frame_wait_ms = 0;      // init done -> first frame picked up
    double first_detection_ms = 0;       // first frame -> first detection done
    double first_command_ms = 0;         // first detection -> first command
    double time_to_first_command_ms = 0; // process start -> first command
};

class Metrics {
public:
    // Reserve sample capacity for ~fps*duration frames so the hot path never
    // reallocates.
    void reserve(size_t expected_frames);

    void on_capture() { ++captured_; }
    // Override the captured count from the camera's own counter (live mode, where
    // frames are dropped by the latest-frame slot and not all reach perception).
    void set_captured(uint64_t c) { captured_ = c; }

    // One frame finished perception. `f2d_ms` = capture->detection latency.
    void on_processed(double f2d_ms, bool had_ball, bool had_bucket);

    // A control decision produced a command. `f2c_ms` = capture->command.
    void on_command(double f2c_ms) { f2c_ms_.push_back(f2c_ms); }

    void on_motor_command() { ++motor_commands_; }
    void on_arm_command() { ++arm_commands_; }
    void on_decode_error() { ++decode_errors_; }
    void on_inference_error() { ++inference_errors_; }
    // Incomplete camera frame (USB-ISO short-packet) that was dropped before
    // inference — a capture-reliability stat, NOT an inference error.
    void on_short_frame() { ++short_frames_; }

    // --- Deep profiling (board live path) ---

    // Record one frame's per-stage timings (and append a CSV row if open).
    void on_stage_timing(const StageTiming &t);
    // Record which CPU the inference loop ran on (sched_getcpu()).
    void record_core(int cpu);
    void set_cold_start(const ColdStart &c) {
        cold_ = c;
        have_cold_ = true;
    }
    // Open the per-frame CSV (--profile-csv). Returns false if it can't be opened.
    bool open_csv(const char *path);
    void close_csv();
    // Read VmRSS/VmHWM/meminfo, track the peak, and emit a TENNIS_RES line.
    void sample_resource(double elapsed_sec);

    // --- Emitters ---
    void emit_result(double duration_sec) const;    // TENNIS_BENCH_RESULT (+ tails)
    void emit_profile_result() const;               // TENNIS_BENCH_PROFILE_RESULT
    void emit_pipeline_result() const;              // TENNIS_PIPELINE_RESULT
    void emit_cold_start() const;                   // TENNIS_COLD_START
    void emit_core_hist(const char *affinity) const; // TENNIS_CORE_HIST

private:
    uint64_t captured_ = 0;
    uint64_t processed_ = 0;
    uint64_t detections_ = 0;
    uint64_t bucket_detections_ = 0;
    uint64_t motor_commands_ = 0;
    uint64_t arm_commands_ = 0;
    uint64_t decode_errors_ = 0;
    uint64_t inference_errors_ = 0;
    uint64_t short_frames_ = 0;
    std::vector<double> f2d_ms_; // capture -> detection
    std::vector<double> f2c_ms_; // capture -> command

    // Per-stage samples (deep profile).
    std::vector<double> queue_age_ms_;
    std::vector<double> decode_ms_;
    std::vector<double> letterbox_ms_;
    std::vector<double> inputs_set_ms_;
    std::vector<double> run_ms_;
    std::vector<double> rknn_perf_run_ms_;
    std::vector<double> outputs_get_ms_;
    std::vector<double> postprocess_ms_;
    std::vector<double> control_ms_;

    long core_hist_[kMaxCpus] = {0};
    ColdStart cold_{};
    bool have_cold_ = false;
    size_t rss_hwm_kb_ = 0;
    FILE *csv_ = nullptr;
};

} // namespace tennis
