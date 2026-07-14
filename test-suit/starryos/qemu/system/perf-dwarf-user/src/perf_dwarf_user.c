/*
 * perf_dwarf_user.c -- PERF_SAMPLE_REGS_USER + PERF_SAMPLE_STACK_USER test.
 *
 * Proves the kernel captures the interrupted user register file and a bounded
 * dump of the user stack, so a host `perf report --call-graph dwarf` can unwind
 * `-fomit-frame-pointer` binaries (where frame-pointer unwinding fails). The
 * workload is built `-fomit-frame-pointer` on purpose: this feature does NOT rely
 * on frame records -- it hands the raw regs + stack bytes to the host unwinder.
 *
 * Flow mirrors perf-hw-callchain-user: open a PERF_TYPE_RAW/config=0x11 sampling
 * event with sample_type = IP|TID|TIME|REGS_USER|STACK_USER, sample_regs_user set
 * to the full PERF_REG_ARM64 mask (x0..x30, SP, PC) and sample_stack_user = 1 KiB,
 * mmap the ring, ENABLE, run a syscall-heavy loop, DISABLE, then parse each
 * PERF_RECORD_SAMPLE body in canonical field order:
 *     u64 ip; u32 pid; u32 tid; u64 time;
 *     u64 abi; u64 regs[popcount(mask)];        (REGS_USER; regs only if abi != 0)
 *     u64 size; u8 data[size]; u64 dyn_size;     (STACK_USER; data/dyn only if size)
 *
 * A user (EL0) sample carries abi == PERF_SAMPLE_REGS_ABI_64 with the full
 * register set and a non-empty stack dump; a kernel sample carries abi == 0 and
 * size == 0 (shorter body) -- the parser handles both.
 *
 * SUCCESS ==
 *     fd >= 0 AND mmap ok AND the ring is non-empty
 *   AND every record body stays within hdr.size (no overrun)
 *   AND at least one USER sample has abi == ABI_64, a non-zero PC and SP register,
 *       stack size == the requested dump, and 0 < dyn_size <= size (real stack
 *       bytes were captured through the no-fault reader).
 * On success exactly one line `STARRY_PERF_DWARF_USER_OK` is printed.
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
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
#define PERF_SAMPLE_REGS_USER (1ull << 12)
#define PERF_SAMPLE_STACK_USER (1ull << 13)

/* aarch64 PERF_REG_ARM64: x0..x30 = 0..30, SP = 31, PC = 32; MAX = 33. */
#define PERF_REG_ARM64_SP 31u
#define PERF_REG_ARM64_PC 32u
#define PERF_REG_ARM64_MAX 33u
#define PERF_REG_ARM64_MASK (((uint64_t)1 << PERF_REG_ARM64_MAX) - 1)

#define PERF_SAMPLE_REGS_ABI_NONE 0ull
#define PERF_SAMPLE_REGS_ABI_64 2ull

#define SAMPLE_PERIOD 100000ull
#define SAMPLE_STACK_USER_SIZE 1024u

#ifndef PERF_EVENT_IOC_ENABLE
#define PERF_EVENT_IOC_ENABLE 0x2400u
#endif
#ifndef PERF_EVENT_IOC_DISABLE
#define PERF_EVENT_IOC_DISABLE 0x2401u
#endif
#ifndef PERF_EVENT_IOC_RESET
#define PERF_EVENT_IOC_RESET 0x2403u
#endif
#ifndef PERF_RECORD_SAMPLE
#define PERF_RECORD_SAMPLE 9u
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

#define PERF_ATTR_FLAG_DISABLED (1ull << 0)

struct perf_event_mmap_page {
    uint32_t version;
    uint32_t compat_version;
    uint32_t lock;
    uint32_t index;
    int64_t offset;
    uint64_t time_enabled;
    uint64_t time_running;
    union {
        uint64_t capabilities;
        struct {
            uint64_t cap_bit0 : 1, cap_bit0_is_deprecated : 1,
                cap_user_rdpmc : 1, cap_user_time : 1, cap_user_time_zero : 1,
                cap_user_time_short : 1, cap_____res : 58;
        };
    };
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

_Static_assert(offsetof(struct perf_event_attr, sample_type) == 24, "off24");
_Static_assert(offsetof(struct perf_event_attr, sample_regs_user) == 80, "off80");
_Static_assert(offsetof(struct perf_event_attr, sample_stack_user) == 88,
               "off88");
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

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int fail(const char *reason) {
    printf("perf-dwarf-user FAILED: %s\n", reason);
    return 1;
}

static void ring_copy(const uint8_t *base, uint64_t size, uint64_t at, void *dst,
                      size_t n) {
    for (size_t b = 0; b < n; b++) {
        ((uint8_t *)dst)[b] = base[(at + b) % size];
    }
}

/* A syscall-heavy nested call chain (read a page from /dev/zero), NOT pure
 * arithmetic: QEMU-TCG's cycle counter barely advances on ALU-only work, so a
 * plain loop overflows the sampling counter almost never. read() does real work
 * the counter tracks, so samples land steadily -- some in user (the loop) with a
 * valid SP/PC and stack to dump. Built -fomit-frame-pointer on purpose. */
static volatile uint64_t g_sink;
static int g_zfd = -1;

__attribute__((noinline)) static void busy(void) {
    static uint8_t buf[4096];
    for (uint64_t i = 0; i < 400000ull; i++) {
        if (g_zfd >= 0) {
            if (read(g_zfd, buf, sizeof(buf)) < 0) {
                break;
            }
        } else {
            g_sink += i * 3ull + 1ull;
        }
    }
}
__attribute__((noinline)) static void inner(void) { busy(); }
__attribute__((noinline)) static void mid(void) { inner(); }
__attribute__((noinline)) static void outer(void) { mid(); }

int main(void) {
#if !defined(__aarch64__)
    /* Hardware-PMU perf is aarch64-only (ARM PMUv3); skip-as-pass elsewhere. */
    printf("STARRY_PERF_DWARF_USER_OK\n");
    return 0;
#endif
    struct perf_event_attr attr;
    for (size_t i = 0; i < sizeof(attr); i++) {
        ((volatile unsigned char *)&attr)[i] = 0;
    }
    attr.type = PERF_TYPE_RAW;
    attr.config = ARM_PMU_EVT_CPU_CYCLES;
    attr.size = (uint32_t)sizeof(struct perf_event_attr);
    attr.sample_period = SAMPLE_PERIOD;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                       PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
    attr.sample_regs_user = PERF_REG_ARM64_MASK;
    attr.sample_stack_user = SAMPLE_STACK_USER_SIZE;
    attr.read_format = 0;
    attr.flags = PERF_ATTR_FLAG_DISABLED;

    long fd = perf_event_open(&attr, 0, -1, -1, 0ul);
    if (fd < 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "perf_event_open(dwarf) errno=%d", errno);
        return fail(msg);
    }
    int efd = (int)fd;

    void *base = mmap(NULL, PERF_MMAP_TOTAL_BYTES, PROT_READ | PROT_WRITE,
                      MAP_SHARED, efd, 0);
    if (base == MAP_FAILED) {
        int e = errno;
        char msg[96];
        snprintf(msg, sizeof(msg), "mmap ring errno=%d", e);
        close(efd);
        return fail(msg);
    }
    struct perf_event_mmap_page *meta = (struct perf_event_mmap_page *)base;

    g_zfd = open("/dev/zero", O_RDONLY);

    (void)ioctl(efd, PERF_EVENT_IOC_RESET, 0);
    (void)ioctl(efd, PERF_EVENT_IOC_ENABLE, 0);
    outer();
    (void)ioctl(efd, PERF_EVENT_IOC_DISABLE, 0);
    if (g_zfd >= 0) {
        close(g_zfd);
    }

    uint64_t data_head = meta->data_head;
    __sync_synchronize();
    uint64_t data_tail = meta->data_tail;
    uint64_t data_offset = meta->data_offset;
    uint64_t data_size = meta->data_size;
    const uint8_t *data_base = (const uint8_t *)base + data_offset;

    uint64_t sample_count = 0;
    uint64_t user_regs_samples = 0; /* abi == ABI_64 with a plausible SP/PC */
    uint64_t good_samples = 0;      /* + a real (dyn_size > 0) stack dump */
    int bad_record = 0;

    uint64_t off = data_tail;
    while (off < data_head && data_size != 0) {
        uint64_t rel = off % data_size;
        struct perf_event_header hdr;
        ring_copy(data_base, data_size, rel, &hdr, sizeof(hdr));
        if (hdr.size == 0 || off + hdr.size > data_head) {
            bad_record = 1;
            break;
        }
        if (hdr.type == PERF_RECORD_SAMPLE) {
            sample_count++;
            /* Body: ip(8) pid+tid(8) time(8), then REGS_USER, then STACK_USER. */
            uint64_t cur = (uint64_t)sizeof(hdr) + 8 + 8 + 8;
            /* REGS_USER: u64 abi, then popcount(mask) regs iff abi != 0. */
            if (cur + 8 > hdr.size) {
                bad_record = 1;
                break;
            }
            uint64_t abi = 0;
            ring_copy(data_base, data_size, (rel + cur) % data_size, &abi, 8);
            cur += 8;
            uint64_t sp = 0, pc = 0;
            int have_regs = 0;
            if (abi == PERF_SAMPLE_REGS_ABI_64) {
                if (cur + PERF_REG_ARM64_MAX * 8 > hdr.size) {
                    bad_record = 1;
                    break;
                }
                ring_copy(data_base, data_size,
                          (rel + cur + PERF_REG_ARM64_SP * 8) % data_size, &sp,
                          8);
                ring_copy(data_base, data_size,
                          (rel + cur + PERF_REG_ARM64_PC * 8) % data_size, &pc,
                          8);
                cur += PERF_REG_ARM64_MAX * 8;
                have_regs = 1;
            }
            /* STACK_USER: u64 size, then size bytes + u64 dyn_size iff size != 0. */
            if (cur + 8 > hdr.size) {
                bad_record = 1;
                break;
            }
            uint64_t ssize = 0;
            ring_copy(data_base, data_size, (rel + cur) % data_size, &ssize, 8);
            cur += 8;
            uint64_t dyn = 0;
            int have_stack = 0;
            if (ssize != 0) {
                if (cur + ssize + 8 > hdr.size) {
                    bad_record = 1;
                    break;
                }
                cur += ssize;
                ring_copy(data_base, data_size, (rel + cur) % data_size, &dyn,
                          8);
                cur += 8;
                have_stack = 1;
            }
            if (have_regs && sp != 0 && pc != 0) {
                user_regs_samples++;
                if (have_stack && ssize == SAMPLE_STACK_USER_SIZE && dyn > 0 &&
                    dyn <= ssize) {
                    good_samples++;
                }
            }
        }
        off += hdr.size;
    }

    printf("STARRY_PERF_DWARF_USER samples=%llu uregs=%llu good=%llu bad=%d "
           "sink=%llu\n",
           (unsigned long long)sample_count,
           (unsigned long long)user_regs_samples,
           (unsigned long long)good_samples, bad_record,
           (unsigned long long)g_sink);

    int rc = 0;
    if (data_head == data_tail) {
        rc = fail("no samples captured (data_head == data_tail)");
    } else if (sample_count == 0) {
        rc = fail("no PERF_RECORD_SAMPLE records in ring");
    } else if (bad_record) {
        rc = fail("a record field overran hdr.size (corrupt REGS/STACK block)");
    } else if (user_regs_samples == 0) {
        rc = fail("no user sample carried ABI_64 regs with non-zero SP/PC");
    } else if (good_samples == 0) {
        rc = fail("no user sample carried a non-empty STACK_USER dump");
    }

    (void)munmap(base, PERF_MMAP_TOTAL_BYTES);
    close(efd);

    if (rc == 0) {
        printf("STARRY_PERF_DWARF_USER_OK\n");
        return 0;
    }
    return rc;
}
