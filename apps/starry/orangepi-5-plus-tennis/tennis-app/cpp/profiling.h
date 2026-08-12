// SPDX-License-Identifier: Apache-2.0
//
// CPU-affinity + core-residency helpers shared by every benchmark mode. They are
// real on Linux/StarryOS and no-op stubs elsewhere (the macOS host dry-run
// build), so the same call sites compile everywhere. The including translation
// unit must `#define _GNU_SOURCE` before its first include for sched_getcpu /
// sched_setaffinity / the CPU_* macros to be visible.
#pragma once

#include <cstdio>

#include "bench/metrics.h" // kMaxCpus

#if defined(__linux__)
#include <sched.h>

#include <cstdlib>
#include <cstring>
#endif

namespace tennis {

#if defined(__linux__)

// Parse a cpu list ("4", "4-7", "0-3", "0,4,5") into a cpu_set_t.
// Returns the count selected, or -1 on parse error.
inline int parse_cpulist(const char *s, cpu_set_t *set) {
    CPU_ZERO(set);
    int count = 0;
    const char *p = s;
    while (*p != '\0') {
        char *e = nullptr;
        long lo = std::strtol(p, &e, 10);
        if (e == p || lo < 0 || lo >= kMaxCpus) return -1;
        long hi = lo;
        if (*e == '-') {
            p = e + 1;
            hi = std::strtol(p, &e, 10);
            if (e == p || hi < lo || hi >= kMaxCpus) return -1;
        }
        for (long c = lo; c <= hi; ++c) {
            if (!CPU_ISSET(static_cast<int>(c), set)) {
                CPU_SET(static_cast<int>(c), set);
                ++count;
            }
        }
        p = e;
        if (*p == ',') {
            ++p;
        } else if (*p != '\0') {
            return -1;
        }
    }
    return count > 0 ? count : -1;
}

// Pin the calling (inference/poll) thread to `cpulist`, verify the kernel
// migrated us, and emit one machine-readable TENNIS_AFFINITY line. On StarryOS
// this is the gate for every affinity-dependent number: if sched_setaffinity is
// not honored, result=FAIL. Call AFTER model + camera init (NPU init hangs on a
// non-boot core).
inline bool apply_and_check_affinity(const char *cpulist) {
    cpu_set_t set;
    int selected = parse_cpulist(cpulist, &set);
    if (selected < 0) {
        std::printf("TENNIS_AFFINITY requested=%s result=PARSE_ERROR\n", cpulist);
        return false;
    }
    int set_ret = sched_setaffinity(0, sizeof(set), &set);
    for (int i = 0; i < 64; ++i) sched_yield();
    int cpu_now = sched_getcpu();
    bool on_target =
        cpu_now >= 0 && cpu_now < kMaxCpus && CPU_ISSET(cpu_now, &set);
    std::printf("TENNIS_AFFINITY requested=%s selected_cpus=%d "
                "setaffinity_ret=%d cpu_now=%d result=%s\n",
                cpulist, selected, set_ret, cpu_now,
                (set_ret == 0 && on_target) ? "PASS" : "FAIL");
    return set_ret == 0 && on_target;
}

inline int profiler_getcpu() { return sched_getcpu(); }

#else // non-Linux host (macOS dry-run build)

inline bool apply_and_check_affinity(const char *cpulist) {
    std::printf("TENNIS_AFFINITY requested=%s result=UNSUPPORTED\n", cpulist);
    return false;
}
inline int profiler_getcpu() { return -1; }

#endif

} // namespace tennis
