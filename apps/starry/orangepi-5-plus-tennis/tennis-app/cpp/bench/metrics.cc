// SPDX-License-Identifier: Apache-2.0
#include "metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>

namespace tennis {

double percentile_ms(std::vector<double> samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    if (samples.size() == 1) return samples[0];
    if (p <= 0.0) return samples.front();
    if (p >= 1.0) return samples.back();
    const double rank = p * static_cast<double>(samples.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(rank));
    const double frac = rank - static_cast<double>(lo);
    return samples[lo] * (1.0 - frac) + samples[lo + 1] * frac;
}

static double avg_ms(const std::vector<double> &v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}

size_t read_rss_kb() {
    // Preferred: VmRSS from /proc/self/status (Linux / StarryOS).
    if (FILE *f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        size_t kb = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "VmRSS:", 6) == 0) {
                kb = static_cast<size_t>(std::strtoul(line + 6, nullptr, 10));
                break;
            }
        }
        std::fclose(f);
        if (kb) return kb;
    }
    // Fallback: getrusage max RSS (KiB on Linux, bytes on macOS/BSD).
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__APPLE__)
        return static_cast<size_t>(ru.ru_maxrss) / 1024;
#else
        return static_cast<size_t>(ru.ru_maxrss);
#endif
    }
    return 0;
}

void Metrics::reserve(size_t expected_frames) {
    f2d_ms_.reserve(expected_frames);
    f2c_ms_.reserve(expected_frames);
}

void Metrics::on_processed(double f2d_ms, bool had_ball, bool had_bucket) {
    ++processed_;
    f2d_ms_.push_back(f2d_ms);
    if (had_ball) ++detections_;
    if (had_bucket) ++bucket_detections_;
}

void Metrics::emit_result(double duration_sec) const {
    std::printf(
        "TENNIS_BENCH_RESULT duration_sec=%.3f captured=%llu processed=%llu "
        "detections=%llu bucket_detections=%llu virtual_motor_commands=%llu "
        "virtual_arm_commands=%llu frame_to_detection_ms_avg=%.3f "
        "frame_to_detection_ms_p50=%.3f frame_to_detection_ms_p95=%.3f "
        "frame_to_command_ms_avg=%.3f frame_to_command_ms_p50=%.3f "
        "frame_to_command_ms_p95=%.3f decode_errors=%llu inference_errors=%llu "
        "memory_rss_kb=%zu\n",
        duration_sec, static_cast<unsigned long long>(captured_),
        static_cast<unsigned long long>(processed_),
        static_cast<unsigned long long>(detections_),
        static_cast<unsigned long long>(bucket_detections_),
        static_cast<unsigned long long>(motor_commands_),
        static_cast<unsigned long long>(arm_commands_), avg_ms(f2d_ms_),
        percentile_ms(f2d_ms_, 0.50), percentile_ms(f2d_ms_, 0.95),
        avg_ms(f2c_ms_), percentile_ms(f2c_ms_, 0.50),
        percentile_ms(f2c_ms_, 0.95),
        static_cast<unsigned long long>(decode_errors_),
        static_cast<unsigned long long>(inference_errors_), read_rss_kb());
}

} // namespace tennis
