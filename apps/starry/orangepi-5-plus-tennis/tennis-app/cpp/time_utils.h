// SPDX-License-Identifier: Apache-2.0
//
// Monotonic timing helpers shared by the capture/perception/control stages and
// the benchmark. A single monotonic clock keeps capture->detection->command
// latency deltas meaningful regardless of wall-clock adjustments.
#pragma once

#include <cstdint>
#include <ctime>

namespace tennis {

inline int64_t monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL +
           static_cast<int64_t>(ts.tv_nsec);
}

inline double ns_to_ms(int64_t ns) { return static_cast<double>(ns) / 1.0e6; }

// Sleep for the given number of nanoseconds (best-effort; used to pace the
// dry-run simulation to a target FPS). Never called on a latency-critical path.
inline void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    struct timespec req;
    req.tv_sec = static_cast<time_t>(ns / 1000000000LL);
    req.tv_nsec = static_cast<long>(ns % 1000000000LL);
    nanosleep(&req, nullptr);
}

} // namespace tennis
