// SPDX-License-Identifier: Apache-2.0
//
// Benchmark accumulator. The benchmark line is the primary deliverable, so the
// hot path must stay allocation-free: per-frame latency samples are pushed into
// vectors reserved up front, running totals are plain integers, and percentiles
// are computed once (sort-on-a-copy) at the end of the run.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tennis {

// p in [0,1]; linear-interpolated percentile over a copy of `samples`
// (`samples` is taken by value so the caller's vector is left untouched).
double percentile_ms(std::vector<double> samples, double p);

// Resident set size in KiB (VmRSS on Linux/StarryOS; getrusage fallback
// elsewhere). 0 if unavailable.
size_t read_rss_kb();

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

    // Emit the mandatory TENNIS_BENCH_RESULT line.
    void emit_result(double duration_sec) const;

private:
    uint64_t captured_ = 0;
    uint64_t processed_ = 0;
    uint64_t detections_ = 0;
    uint64_t bucket_detections_ = 0;
    uint64_t motor_commands_ = 0;
    uint64_t arm_commands_ = 0;
    uint64_t decode_errors_ = 0;
    uint64_t inference_errors_ = 0;
    std::vector<double> f2d_ms_; // capture -> detection
    std::vector<double> f2c_ms_; // capture -> command
};

} // namespace tennis
