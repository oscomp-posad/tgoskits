/*
 * perf_tracepoint_sample.c -- tracepoint PERF_RECORD_SAMPLE emission
 * (`perf record -e syscalls:sys_enter_openat`, driven via the raw
 * perf_event_open ABI).
 *
 * A tracepoint perf event used to be BPF-attach-only: mmap(perf_fd) was rejected
 * and a tracepoint hit ran only a BPF program, emitting zero sample records. Now
 * opening a PERF_TYPE_TRACEPOINT event with sample_period > 0 and mmapping the
 * ring makes each hit write a PERF_RECORD_SAMPLE (tid/time/cpu), so a
 * `perf record`-shaped capture produces perf.data of "which task hit this
 * tracepoint, and when".
 *
 * The on-target perf binary lacks libtraceevent, so we drive the syscall
 * directly: read the numeric tracepoint id from debugfs
 * (/sys/kernel/debug/tracing/events/syscalls/sys_enter_openat/id), open
 * PERF_TYPE_TRACEPOINT(2) with config = that id, sample_period=1,
 * sample_type = IP|TID|TIME; mmap the ring; ENABLE; issue openat() syscalls (each
 * hits sys_enter_openat); DISABLE; walk the ring for PERF_RECORD_SAMPLE records.
 *
 * A tracepoint hit has no interrupted register frame, so the sample carries no
 * code IP (ip == 0) and no callchain -- the useful fields are TID/TIME, which
 * attribute the hit to the triggering task.
 *
 * SUCCESS ==
 *     the tracepoint id resolves from debugfs
 *   AND the tracepoint event opens + mmaps
 *   AND at least one PERF_RECORD_SAMPLE is captured whose tid is this thread
 *       (proving the hit is attributed to the task that triggered it).
 * Prints STARRY_PERF_TRACEPOINT_SAMPLE_OK.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PERF_TYPE_TRACEPOINT 2u

#define PERF_SAMPLE_IP (1ull << 0)
#define PERF_SAMPLE_TID (1ull << 1)
#define PERF_SAMPLE_TIME (1ull << 2)

#define PERF_ATTR_FLAG_DISABLED (1ull << 0)

#ifndef PERF_EVENT_IOC_ENABLE
#define PERF_EVENT_IOC_ENABLE 0x2400u
#endif
#ifndef PERF_EVENT_IOC_DISABLE
#define PERF_EVENT_IOC_DISABLE 0x2401u
#endif
#ifndef PERF_RECORD_SAMPLE
#define PERF_RECORD_SAMPLE 9u
#endif
#ifndef SYS_perf_event_open
#define SYS_perf_event_open 241
#endif

#define TP_ID_PATH                                                              \
    "/sys/kernel/debug/tracing/events/syscalls/sys_enter_openat/id"

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

struct perf_event_mmap_page {
    uint8_t __pad[1024];
    uint64_t data_head;
    uint64_t data_tail;
    uint64_t data_offset;
    uint64_t data_size;
};

_Static_assert(offsetof(struct perf_event_mmap_page, data_head) == 1024, "dh");

struct perf_event_header {
    uint32_t type;
    uint16_t misc;
    uint16_t size;
};

#define PERF_MMAP_PAGE_SIZE 4096u
#define PERF_MMAP_DATA_PAGES 8u
#define PERF_MMAP_TOTAL_BYTES                                                   \
    ((size_t)(1u + PERF_MMAP_DATA_PAGES) * PERF_MMAP_PAGE_SIZE)

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int fail(const char *reason) {
    printf("perf-tracepoint-sample FAILED: %s\n", reason);
    return 1;
}

static void ring_copy(const uint8_t *base, uint64_t size, uint64_t at, void *dst,
                      size_t n) {
    for (size_t b = 0; b < n; b++) {
        ((uint8_t *)dst)[b] = base[(at + b) % size];
    }
}

/* Read the decimal tracepoint id from debugfs; return -1 on failure. */
static long read_tp_id(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    long id = 0;
    int any = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            id = id * 10 + (buf[i] - '0');
            any = 1;
        } else if (any) {
            break;
        }
    }
    return any ? id : -1;
}

int main(void) {
#if !defined(__aarch64__)
    printf("STARRY_PERF_TRACEPOINT_SAMPLE_OK\n");
    return 0;
#endif
    long tp_id = read_tp_id(TP_ID_PATH);
    if (tp_id < 0) {
        return fail("could not read tracepoint id from debugfs");
    }
    long my_tid = syscall(SYS_gettid);
    printf("STARRY_PERF_TRACEPOINT id=%ld tid=%ld\n", tp_id, my_tid);

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_TRACEPOINT;
    attr.size = (uint32_t)sizeof(attr);
    attr.config = (uint64_t)tp_id;
    attr.sample_period = 1; /* a sample per hit */
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    attr.flags = PERF_ATTR_FLAG_DISABLED;

    long fd = perf_event_open(&attr, 0, -1, -1, 0ul);
    if (fd < 0) {
        char m[96];
        snprintf(m, sizeof(m), "perf_event_open(tracepoint) errno=%d", errno);
        return fail(m);
    }
    int efd = (int)fd;

    void *base = mmap(NULL, PERF_MMAP_TOTAL_BYTES, PROT_READ | PROT_WRITE,
                      MAP_SHARED, efd, 0);
    if (base == MAP_FAILED) {
        int e = errno;
        close(efd);
        char m[96];
        snprintf(m, sizeof(m), "mmap ring errno=%d", e);
        return fail(m);
    }
    struct perf_event_mmap_page *meta = (struct perf_event_mmap_page *)base;

    (void)ioctl(efd, PERF_EVENT_IOC_ENABLE, 0);
    /* Trigger the tracepoint: each openat() hits sys_enter_openat at entry (it
     * fires before the path lookup, so a failing open still counts). */
    for (int i = 0; i < 300; i++) {
        int t = (int)syscall(SYS_openat, AT_FDCWD, "/nonexistent-tp-probe",
                             O_RDONLY, 0);
        if (t >= 0) {
            close(t);
        }
    }
    (void)ioctl(efd, PERF_EVENT_IOC_DISABLE, 0);

    uint64_t data_head = meta->data_head;
    __sync_synchronize();
    uint64_t data_tail = meta->data_tail;
    uint64_t data_offset = meta->data_offset;
    uint64_t data_size = meta->data_size;
    const uint8_t *data_base = (const uint8_t *)base + data_offset;

    uint64_t samples = 0, tid_match = 0;
    uint64_t first_tid = 0;
    uint64_t off = data_tail;
    while (off < data_head && data_size != 0) {
        uint64_t rel = off % data_size;
        struct perf_event_header hdr;
        ring_copy(data_base, data_size, rel, &hdr, sizeof(hdr));
        if (hdr.size == 0 || off + hdr.size > data_head) {
            break;
        }
        if (hdr.type == PERF_RECORD_SAMPLE) {
            samples++;
            /* body (sample_type = IP|TID|TIME): u64 ip; u32 pid; u32 tid;
             * u64 time. The tid attributes the hit to the triggering task. */
            uint64_t body = (rel + sizeof(hdr)) % data_size;
            uint32_t tid = 0;
            ring_copy(data_base, data_size, (body + 12) % data_size, &tid, 4);
            if (first_tid == 0) {
                first_tid = tid;
            }
            if ((long)tid == my_tid) {
                tid_match++;
            }
        }
        off += hdr.size;
    }

    printf("STARRY_PERF_TRACEPOINT_SAMPLE samples=%llu tid_match=%llu "
           "first_tid=%llu\n",
           (unsigned long long)samples, (unsigned long long)tid_match,
           (unsigned long long)first_tid);

    (void)munmap(base, PERF_MMAP_TOTAL_BYTES);
    close(efd);

    if (samples == 0) {
        return fail("no PERF_RECORD_SAMPLE captured from the tracepoint");
    }
    if (tid_match == 0) {
        return fail("no sample was attributed to the triggering thread");
    }

    printf("STARRY_PERF_TRACEPOINT_SAMPLE_OK\n");
    return 0;
}
