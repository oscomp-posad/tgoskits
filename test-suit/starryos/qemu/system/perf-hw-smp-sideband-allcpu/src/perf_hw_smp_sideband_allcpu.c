/*
 * perf_hw_smp_sideband_allcpu.c -- system-wide (`perf record -a`) side-band test.
 *
 * `perf record -a` opens one sampling event PER CPU (pid=-1, cpu=i). For the
 * capture to be symbolizable, each per-CPU ring must also receive the kernel
 * side-band records -- PERF_RECORD_COMM / MMAP2 / FORK / EXIT -- for whatever
 * process runs on that core, not just for a single monitored task. Previously the
 * side-band hooks fanned out only to per-task event rings, so a `-a` capture got
 * ZERO of these and could not symbolize newly-loaded DSOs.
 *
 * This test drives exactly that: open one `-a` sampling event per cpu with
 * attr.comm = attr.mmap2 = attr.task = attr.sample_id_all = 1, mmap + ENABLE all
 * rings, THEN fork a child that execs itself in `--busy` mode and exits. That
 * post-enable activity emits FORK (at the clone), COMM + MMAP2 (at the child's
 * execve) and EXIT (at the child's exit) into whichever core's `-a` ring the work
 * ran on. The parent drains ALL rings and confirms the records arrived.
 *
 * SUCCESS ==
 *     at least one `-a` ring opened + mmapped
 *   AND across all rings: a COMM record (non-empty name), an MMAP2 record
 *       (non-empty filename), and at least one FORK and one EXIT record.
 * On success exactly one line `STARRY_PERF_SMP_SIDEBAND_OK` is printed.
 *
 * All ABI structs are defined locally (no <linux/perf_event.h> dependency).
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
#include <sys/wait.h>
#include <unistd.h>

#ifndef PERF_TYPE_RAW
#define PERF_TYPE_RAW 4u
#endif
#ifndef ARM_PMU_EVT_CPU_CYCLES
#define ARM_PMU_EVT_CPU_CYCLES 0x11ull
#endif

#define PERF_SAMPLE_IP (1ull << 0)
#define PERF_SAMPLE_TID (1ull << 1)
#define PERF_SAMPLE_TIME (1ull << 2)

/* perf_event_attr.flags bits (this kbpf ABI: mmap2 = bit 23). */
#define PERF_ATTR_FLAG_DISABLED (1ull << 0)
#define PERF_ATTR_FLAG_COMM (1ull << 9)
#define PERF_ATTR_FLAG_TASK (1ull << 13)
#define PERF_ATTR_FLAG_SAMPLE_ID_ALL (1ull << 18)
#define PERF_ATTR_FLAG_MMAP2 (1ull << 23)

#define PERF_RECORD_EXIT 4u
#define PERF_RECORD_FORK 7u
#define PERF_RECORD_COMM 3u
#define PERF_RECORD_MMAP2 10u
#define PERF_RECORD_SAMPLE 9u

/* COMM body: header(8) + u32 pid + u32 tid, then the name. */
#define COMM_NAME_OFF 16u
/* MMAP2 body: header(8) + pid/tid(8) + addr/len/pgoff(24) + maj/min(8) +
 * ino(8) + ino_generation(8) + prot/flags(8), then the filename. */
#define MMAP2_FILENAME_OFF 72u

/* The `-a` event must be a sampler (period > 0) to allocate a ring and hit the
 * fan-out path, but this test only cares about side-band records. Use the maximum
 * 32-bit period so samples are effectively never generated: otherwise a sample
 * flood fills the ring and the late EXIT record hits back-pressure (`ring_write`
 * drops NEW records when full, like Linux) since the test does not drain during
 * the run (real `perf record -a` poll-drains continuously). */
#define SAMPLE_PERIOD 0xFFFFFFFFull

/* Matches the qemu/system group's smp4 config; extra cpus that fail to open are
 * skipped, so the test still runs (all activity on cpu 0) under smp1. */
#define NCPU 4

#ifndef PERF_EVENT_IOC_ENABLE
#define PERF_EVENT_IOC_ENABLE 0x2400u
#endif
#ifndef PERF_EVENT_IOC_DISABLE
#define PERF_EVENT_IOC_DISABLE 0x2401u
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

struct perf_event_mmap_page {
    uint32_t version;
    uint32_t compat_version;
    uint32_t lock;
    uint32_t index;
    int64_t offset;
    uint64_t time_enabled;
    uint64_t time_running;
    uint64_t capabilities;
    uint16_t pmc_width;
    uint16_t time_shift;
    uint32_t time_mult;
    uint64_t time_offset;
    uint64_t time_zero;
    uint32_t size;
    uint32_t __reserved_1;
    uint64_t time_cycles;
    uint64_t time_mask;
    uint8_t __reserved[928];
    uint64_t data_head;
    uint64_t data_tail;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t aux_head;
    uint64_t aux_tail;
    uint64_t aux_offset;
    uint64_t aux_size;
};

_Static_assert(offsetof(struct perf_event_mmap_page, data_head) == 1024, "dh");
_Static_assert(offsetof(struct perf_event_mmap_page, data_size) == 1048, "ds");

struct perf_event_header {
    uint32_t type;
    uint16_t misc;
    uint16_t size;
};

#ifndef SYS_perf_event_open
#define SYS_perf_event_open 241
#endif

#define PERF_MMAP_PAGE_SIZE 4096u
#define PERF_MMAP_DATA_PAGES 16u
#define PERF_MMAP_TOTAL_BYTES                                                   \
    ((size_t)(1u + PERF_MMAP_DATA_PAGES) * PERF_MMAP_PAGE_SIZE)

static long peo(struct perf_event_attr *a, pid_t pid, int cpu, int gfd,
                unsigned long fl) {
    return syscall(SYS_perf_event_open, a, pid, cpu, gfd, fl);
}

static int fail(const char *reason) {
    printf("perf-smp-sideband FAILED: %s\n", reason);
    return 1;
}

static size_t ring_cstr(const uint8_t *data_base, uint64_t data_size, uint64_t at,
                        char *out, size_t outsz) {
    size_t n = 0;
    while (n + 1 < outsz) {
        char c = (char)data_base[(at + n) % data_size];
        if (c == '\0') {
            break;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

/* The re-exec'd child body: a brief syscall workload, then exit. Its execve (to
 * get here) emits COMM + MMAP2; its exit emits EXIT. */
static void busy_child(void) {
    /* Tiny workload: just enough to run briefly, then exit. Kept small (with the
     * near-max sample period) so the per-cpu rings never fill and the EXIT record
     * emitted at this child's exit is not lost to ring back-pressure. */
    int zfd = open("/dev/zero", O_RDONLY);
    static uint8_t buf[4096];
    for (uint64_t i = 0; i < 200ull; i++) {
        if (zfd >= 0) {
            if (read(zfd, buf, sizeof(buf)) < 0) {
                break;
            }
        }
    }
    if (zfd >= 0) {
        close(zfd);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--busy") == 0) {
        busy_child();
        return 0;
    }
#if !defined(__aarch64__)
    /* Hardware-PMU perf is aarch64-only (ARM PMUv3); skip-as-pass elsewhere. */
    printf("STARRY_PERF_SMP_SIDEBAND_OK\n");
    return 0;
#endif

    struct perf_event_attr attr;
    long fds[NCPU];
    void *bases[NCPU];
    int opened = 0;
    for (int i = 0; i < NCPU; i++) {
        fds[i] = -1;
        bases[i] = MAP_FAILED;
    }
    for (int i = 0; i < NCPU; i++) {
        for (size_t b = 0; b < sizeof(attr); b++) {
            ((volatile unsigned char *)&attr)[b] = 0;
        }
        attr.type = PERF_TYPE_RAW;
        attr.config = ARM_PMU_EVT_CPU_CYCLES;
        attr.size = (uint32_t)sizeof(attr);
        attr.sample_period = SAMPLE_PERIOD;
        attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
        attr.flags = PERF_ATTR_FLAG_DISABLED | PERF_ATTR_FLAG_COMM |
                     PERF_ATTR_FLAG_MMAP2 | PERF_ATTR_FLAG_TASK |
                     PERF_ATTR_FLAG_SAMPLE_ID_ALL;
        /* pid=-1 (system-wide), cpu=i: side-band + samples for all of core i. */
        fds[i] = peo(&attr, -1, i, -1, 0);
        if (fds[i] < 0) {
            continue; /* cpu offline / not present: skip, still test the rest. */
        }
        /* mmap BEFORE ENABLE (enabling first would register a zero ring). */
        bases[i] = mmap(NULL, PERF_MMAP_TOTAL_BYTES, PROT_READ | PROT_WRITE,
                        MAP_SHARED, (int)fds[i], 0);
        if (bases[i] == MAP_FAILED) {
            close((int)fds[i]);
            fds[i] = -1;
            continue;
        }
        opened++;
    }
    if (opened == 0) {
        return fail("no -a sampling event could be opened on any cpu");
    }
    for (int i = 0; i < NCPU; i++) {
        if (fds[i] >= 0) {
            (void)ioctl((int)fds[i], PERF_EVENT_IOC_ENABLE, 0);
        }
    }

    /* Post-enable activity: fork (-> FORK) a child that execs itself (-> COMM +
     * MMAP2) and exits (-> EXIT). */
    pid_t child = fork();
    if (child == 0) {
        char *av[] = {argv[0], (char *)"--busy", NULL};
        execv(argv[0], av);
        _exit(127);
    }
    if (child < 0) {
        return fail("fork");
    }
    int status = 0;
    waitpid(child, &status, 0);

    uint64_t n_comm = 0, n_mmap2 = 0, n_fork = 0, n_exit = 0, n_sample = 0;
    char comm_name[64] = {0};
    char mmap_file[256] = {0};

    for (int i = 0; i < NCPU; i++) {
        if (fds[i] < 0 || bases[i] == MAP_FAILED) {
            continue;
        }
        (void)ioctl((int)fds[i], PERF_EVENT_IOC_DISABLE, 0);
        struct perf_event_mmap_page *meta = (struct perf_event_mmap_page *)bases[i];
        uint64_t data_head = meta->data_head;
        __sync_synchronize();
        uint64_t data_tail = meta->data_tail;
        uint64_t data_offset = meta->data_offset;
        uint64_t data_size = meta->data_size;
        const uint8_t *data_base = (const uint8_t *)bases[i] + data_offset;

        uint64_t off = data_tail;
        while (off < data_head && data_size != 0) {
            uint64_t rel = off % data_size;
            struct perf_event_header hdr;
            for (size_t b = 0; b < sizeof(hdr); b++) {
                ((uint8_t *)&hdr)[b] = data_base[(rel + b) % data_size];
            }
            if (hdr.size == 0 || off + hdr.size > data_head) {
                break;
            }
            switch (hdr.type) {
            case PERF_RECORD_COMM:
                if (comm_name[0] == '\0') {
                    ring_cstr(data_base, data_size, rel + COMM_NAME_OFF, comm_name,
                              sizeof(comm_name));
                }
                n_comm++;
                break;
            case PERF_RECORD_MMAP2: {
                char tmp[256];
                size_t len = ring_cstr(data_base, data_size,
                                       rel + MMAP2_FILENAME_OFF, tmp, sizeof(tmp));
                if (len > 0 && mmap_file[0] == '\0') {
                    memcpy(mmap_file, tmp, len + 1);
                }
                n_mmap2++;
                break;
            }
            case PERF_RECORD_FORK:
                n_fork++;
                break;
            case PERF_RECORD_EXIT:
                n_exit++;
                break;
            case PERF_RECORD_SAMPLE:
                n_sample++;
                break;
            default:
                break;
            }
            off += hdr.size;
        }
        (void)munmap(bases[i], PERF_MMAP_TOTAL_BYTES);
        close((int)fds[i]);
    }

    printf("STARRY_PERF_SMP_SIDEBAND opened=%d comm=%llu mmap2=%llu fork=%llu "
           "exit=%llu samples=%llu name='%s' file='%s'\n",
           opened, (unsigned long long)n_comm, (unsigned long long)n_mmap2,
           (unsigned long long)n_fork, (unsigned long long)n_exit,
           (unsigned long long)n_sample, comm_name, mmap_file);

    int rc = 0;
    if (n_comm == 0) {
        rc = fail("no PERF_RECORD_COMM in any -a ring");
    } else if (comm_name[0] == '\0') {
        rc = fail("COMM record has empty name");
    } else if (n_mmap2 == 0) {
        rc = fail("no PERF_RECORD_MMAP2 in any -a ring");
    } else if (mmap_file[0] == '\0') {
        rc = fail("MMAP2 record has empty filename");
    } else if (n_fork == 0) {
        rc = fail("no PERF_RECORD_FORK in any -a ring");
    } else if (n_exit == 0) {
        rc = fail("no PERF_RECORD_EXIT in any -a ring");
    }

    if (rc == 0) {
        printf("STARRY_PERF_SMP_SIDEBAND_OK\n");
        return 0;
    }
    return rc;
}
