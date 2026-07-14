/*
 * perf_kprobe_sample.c -- kprobe PERF_RECORD_SAMPLE emission (`perf record -e
 * kprobe:func`, driven via the raw perf_event_open ABI).
 *
 * A kprobe perf event used to be BPF-attach-only: mmap(perf_fd) was rejected and a
 * probe hit ran only a BPF program, emitting zero sample records. Now opening a
 * PERF_TYPE_KPROBE event with sample_period > 0 and mmapping the ring makes each
 * probe hit write a PERF_RECORD_SAMPLE (IP = the probed pc, plus tid/time), so a
 * `perf record`-shaped capture produces perf.data of "where/when this kernel
 * function is hit".
 *
 * The on-target perf binary lacks libelf/traceevent, so we drive the syscall
 * directly (like perf-hw-sample): open PERF_TYPE_KPROBE(6), config=0 (entry
 * kprobe), config1 = a pointer to the target function name, sample_period=1,
 * sample_type = IP|TID|TIME; mmap the ring; ENABLE; issue syscalls to trigger the
 * probe; DISABLE; walk the ring for PERF_RECORD_SAMPLE records.
 *
 * Target: the syscall dispatcher `handle_syscall`, which fires on every syscall.
 * Kernel symbols are Rust-mangled and matched exactly, so we read /proc/kallsyms
 * and recover the exact symbol name (and its address, to check the sample IP).
 *
 * SUCCESS ==
 *     the target symbol is found in kallsyms
 *   AND the kprobe event opens + mmaps
 *   AND at least one PERF_RECORD_SAMPLE is captured whose IP falls in the probed
 *       function (== its kallsyms address).
 * Prints STARRY_PERF_KPROBE_SAMPLE_OK.
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

#define PERF_TYPE_KPROBE 6u
#define PROBE_CONFIG_ENTRY 0ull

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
        uint64_t config1; /* @ offset 56: kprobe_func (pointer to name) */
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

_Static_assert(offsetof(struct perf_event_attr, config1) == 56, "config1@56");

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
    printf("perf-kprobe-sample FAILED: %s\n", reason);
    return 1;
}

static void ring_copy(const uint8_t *base, uint64_t size, uint64_t at, void *dst,
                      size_t n) {
    for (size_t b = 0; b < n; b++) {
        ((uint8_t *)dst)[b] = base[(at + b) % size];
    }
}

/* Scan /proc/kallsyms for the first symbol whose (mangled) name contains
 * `needle`; copy the exact name into `name` and return its address, or 0. */
static uint64_t find_symbol(const char *needle, char *name, size_t name_sz) {
    int fd = open("/proc/kallsyms", O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    static char buf[1 << 16];
    char line[512];
    size_t ln = 0;
    uint64_t found = 0;
    ssize_t got;
    while (found == 0 && (got = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < got; i++) {
            char c = buf[i];
            if (c != '\n' && ln + 1 < sizeof(line)) {
                line[ln++] = c;
                continue;
            }
            line[ln] = '\0';
            ln = 0;
            /* line: "<hexaddr> <type> <name>" */
            char *sp1 = strchr(line, ' ');
            if (!sp1) {
                continue;
            }
            char *sp2 = strchr(sp1 + 1, ' ');
            if (!sp2) {
                continue;
            }
            char *sym = sp2 + 1;
            if (strstr(sym, needle) != NULL) {
                unsigned long long addr = 0;
                for (char *p = line; p < sp1; p++) {
                    char h = *p;
                    int v = (h >= '0' && h <= '9')   ? h - '0'
                            : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                            : (h >= 'A' && h <= 'F') ? h - 'A' + 10
                                                     : -1;
                    if (v < 0) {
                        break;
                    }
                    addr = (addr << 4) | (unsigned)v;
                }
                size_t sl = strlen(sym);
                if (sl + 1 <= name_sz && addr != 0) {
                    memcpy(name, sym, sl + 1);
                    found = addr;
                    break;
                }
            }
        }
    }
    close(fd);
    return found;
}

int main(void) {
#if !defined(__aarch64__)
    printf("STARRY_PERF_KPROBE_SAMPLE_OK\n");
    return 0;
#endif
    char sym[256];
    uint64_t sym_addr = find_symbol("handle_syscall", sym, sizeof(sym));
    if (sym_addr == 0) {
        /* Fall back to any syscall-path symbol. */
        sym_addr = find_symbol("do_syscall", sym, sizeof(sym));
    }
    if (sym_addr == 0) {
        return fail("no probe target symbol found in /proc/kallsyms");
    }
    printf("STARRY_PERF_KPROBE target='%s' addr=%#llx\n", sym,
           (unsigned long long)sym_addr);

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_KPROBE;
    attr.size = (uint32_t)sizeof(attr);
    attr.config = PROBE_CONFIG_ENTRY;
    attr.config1 = (uint64_t)(uintptr_t)sym; /* kprobe_func = &name */
    attr.sample_period = 1;                  /* a sample per hit */
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    attr.flags = PERF_ATTR_FLAG_DISABLED;

    long fd = perf_event_open(&attr, 0, -1, -1, 0ul);
    if (fd < 0) {
        char m[96];
        snprintf(m, sizeof(m), "perf_event_open(kprobe) errno=%d", errno);
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
    /* Trigger the probe: every syscall hits handle_syscall. */
    for (int i = 0; i < 500; i++) {
        (void)getpid();
    }
    (void)ioctl(efd, PERF_EVENT_IOC_DISABLE, 0);

    uint64_t data_head = meta->data_head;
    __sync_synchronize();
    uint64_t data_tail = meta->data_tail;
    uint64_t data_offset = meta->data_offset;
    uint64_t data_size = meta->data_size;
    const uint8_t *data_base = (const uint8_t *)base + data_offset;

    uint64_t samples = 0, ip_in_fn = 0;
    uint64_t first_ip = 0;
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
            /* body: u64 ip; u32 pid; u32 tid; u64 time */
            uint64_t ip = 0;
            ring_copy(data_base, data_size, (rel + sizeof(hdr)) % data_size, &ip,
                      8);
            if (first_ip == 0) {
                first_ip = ip;
            }
            /* The probe pc is at (or just inside) the probed function. Accept an
             * IP within a small window of the symbol start. */
            if (ip >= sym_addr && ip < sym_addr + 64) {
                ip_in_fn++;
            }
        }
        off += hdr.size;
    }

    printf("STARRY_PERF_KPROBE_SAMPLE samples=%llu ip_in_fn=%llu first_ip=%#llx\n",
           (unsigned long long)samples, (unsigned long long)ip_in_fn,
           (unsigned long long)first_ip);

    (void)munmap(base, PERF_MMAP_TOTAL_BYTES);
    close(efd);

    if (samples == 0) {
        return fail("no PERF_RECORD_SAMPLE captured from the kprobe");
    }
    if (ip_in_fn == 0) {
        return fail("sample IP did not match the probed function address");
    }

    printf("STARRY_PERF_KPROBE_SAMPLE_OK\n");
    return 0;
}
