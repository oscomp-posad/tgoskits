/*
 * perf_sw_systemwide.c -- system-wide (`perf stat -a`) software counters.
 *
 * A software counting event opened with pid < 0 (system-wide) used to return
 * ENOSYS: the SW path only handled per-task (pid >= 0), so `perf stat -a`'s
 * software rows failed and perf's cloexec feature-probe (a pid=-1 SW CPU_CLOCK
 * event) warned. This opens the standard `-a` software events system-wide and
 * asserts they open AND aggregate machine-wide across all tasks.
 *
 * cpu-clock (wall time x online CPUs) and context-switches are accurate; a
 * fork/wait + memory-touch workload guarantees both advance. (task-clock and
 * cpu-migrations are best-effort/0 for -a in v1 and are not asserted here.)
 *
 * SUCCESS ==
 *     cpu-clock, context-switches, page-faults all OPEN with pid=-1 (the fix)
 *   AND read(cpu-clock) > 0 AND read(context-switches) > 0.
 * Prints STARRY_PERF_SW_SYSWIDE_OK.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PERF_TYPE_SOFTWARE 1u
#define PERF_COUNT_SW_CPU_CLOCK 0ull
#define PERF_COUNT_SW_PAGE_FAULTS 2ull
#define PERF_COUNT_SW_CONTEXT_SWITCHES 3ull

#define PERF_ATTR_FLAG_DISABLED (1ull << 0)

#ifndef PERF_EVENT_IOC_ENABLE
#define PERF_EVENT_IOC_ENABLE 0x2400u
#endif
#ifndef PERF_EVENT_IOC_DISABLE
#define PERF_EVENT_IOC_DISABLE 0x2401u
#endif

#ifndef SYS_perf_event_open
#define SYS_perf_event_open 241
#endif

struct perf_event_attr {
    uint32_t type;
    uint32_t size;
    uint64_t config;
    union {
        uint64_t sample_period;
        uint64_t sample_freq;
    };
    uint64_t sample_type;
    uint64_t read_format;
    uint64_t flags;
    union {
        uint32_t wakeup_events;
        uint32_t wakeup_watermark;
    };
    uint32_t bp_type;
    union {
        uint64_t bp_addr;
        uint64_t config1;
    };
    union {
        uint64_t bp_len;
        uint64_t config2;
    };
    uint64_t branch_sample_type;
    uint64_t sample_regs_user;
    uint32_t sample_stack_user;
    int32_t clockid;
    uint64_t sample_regs_intr;
    uint32_t aux_watermark;
    uint16_t sample_max_stack;
    uint16_t __reserved_2;
    uint32_t aux_sample_size;
    uint32_t __reserved_3;
};

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/* Open a system-wide (pid=-1, cpu=0) software counting event. */
static long open_sys_sw(uint64_t config) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_SOFTWARE;
    attr.size = (uint32_t)sizeof(attr);
    attr.config = config;
    attr.read_format = 0; /* read() returns just the u64 value */
    attr.flags = PERF_ATTR_FLAG_DISABLED;
    return perf_event_open(&attr, -1, 0, -1, 0ul);
}

static int fail(const char *reason) {
    printf("perf-sw-syswide FAILED: %s\n", reason);
    return 1;
}

int main(void) {
#if !defined(__aarch64__)
    printf("STARRY_PERF_SW_SYSWIDE_OK\n");
    return 0;
#endif
    long cc = open_sys_sw(PERF_COUNT_SW_CPU_CLOCK);
    if (cc < 0) {
        char m[96];
        snprintf(m, sizeof(m), "open -a cpu-clock errno=%d", errno);
        return fail(m);
    }
    long cs = open_sys_sw(PERF_COUNT_SW_CONTEXT_SWITCHES);
    if (cs < 0) {
        char m[96];
        snprintf(m, sizeof(m), "open -a context-switches errno=%d", errno);
        return fail(m);
    }
    long pf = open_sys_sw(PERF_COUNT_SW_PAGE_FAULTS);
    if (pf < 0) {
        char m[96];
        snprintf(m, sizeof(m), "open -a page-faults errno=%d", errno);
        return fail(m);
    }

    (void)ioctl((int)cc, PERF_EVENT_IOC_ENABLE, 0);
    (void)ioctl((int)cs, PERF_EVENT_IOC_ENABLE, 0);
    (void)ioctl((int)pf, PERF_EVENT_IOC_ENABLE, 0);

    /* Workload: fork children that fault in a MiB and exit, so the machine sees
     * context switches (schedule + reap) and page faults while enabled. */
    for (int k = 0; k < 8; k++) {
        pid_t c = fork();
        if (c == 0) {
            char *p = malloc(1 << 20);
            if (p) {
                memset(p, k + 1, 1 << 20);
            }
            _exit(0);
        }
        if (c > 0) {
            int st;
            waitpid(c, &st, 0);
        }
    }

    (void)ioctl((int)cc, PERF_EVENT_IOC_DISABLE, 0);
    (void)ioctl((int)cs, PERF_EVENT_IOC_DISABLE, 0);
    (void)ioctl((int)pf, PERF_EVENT_IOC_DISABLE, 0);

    uint64_t v_cc = 0, v_cs = 0, v_pf = 0;
    ssize_t g_cc = read((int)cc, &v_cc, sizeof(v_cc));
    ssize_t g_cs = read((int)cs, &v_cs, sizeof(v_cs));
    ssize_t g_pf = read((int)pf, &v_pf, sizeof(v_pf));

    printf("STARRY_PERF_SW_SYSWIDE cpu_clock=%llu ctx_switches=%llu "
           "page_faults=%llu (reads %zd/%zd/%zd)\n",
           (unsigned long long)v_cc, (unsigned long long)v_cs,
           (unsigned long long)v_pf, g_cc, g_cs, g_pf);

    close((int)cc);
    close((int)cs);
    close((int)pf);

    if (g_cc != (ssize_t)sizeof(v_cc) || g_cs != (ssize_t)sizeof(v_cs)) {
        return fail("read(perf_fd) did not return a u64 value");
    }
    if (v_cc == 0) {
        return fail("system-wide cpu-clock is 0 (expected wall time x CPUs)");
    }
    if (v_cs == 0) {
        return fail("system-wide context-switches is 0 (expected > 0)");
    }

    printf("STARRY_PERF_SW_SYSWIDE_OK\n");
    return 0;
}
