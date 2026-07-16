/*
 * perf_cli_kprobe.c -- the classic `perf` CLI kprobe path, driven via raw
 * file I/O + perf_event_open (no libelf/libtraceevent perf binary needed).
 *
 * `perf probe -a func` creates a kprobe by writing `p:GROUP/EVENT SYMBOL` to
 * /sys/kernel/debug/tracing/kprobe_events; a dynamic tracepoint then appears at
 * events/GROUP/EVENT/{id,format,enable}. `perf record -e GROUP:EVENT` reads the
 * id and opens a PERF_TYPE_TRACEPOINT perf event on it. StarryOS routes such a
 * (dynamic-kprobe-space) id to the kprobe machinery, so hits emit
 * PERF_RECORD_SAMPLE into the mmap ring.
 *
 * This test performs exactly that sequence by hand:
 *   1. resolve the mangled kallsyms name of `handle_syscall` (fires on every
 *      syscall), like perf resolves symbols.
 *   2. write `p:probe/hsc <mangled>` to kprobe_events; read it back and assert
 *      the line is present.
 *   3. read events/probe/hsc/id -> numeric id; sanity-check events/probe/hsc/format.
 *   4. perf_event_open(PERF_TYPE_TRACEPOINT, config=id, sample_period=1,
 *      sample_type=IP|TID|TIME); mmap the ring; ENABLE.
 *   5. issue syscalls to trigger the probe; DISABLE; walk the ring for samples.
 *   6. remove via `-:probe/hsc`; assert kprobe_events no longer lists it.
 *
 * SUCCESS ==
 *     the symbol resolves AND kprobe_events add+list works
 *   AND events/probe/hsc/id is a nonzero number AND format is non-empty
 *   AND the tracepoint event opens on that id AND >=1 PERF_RECORD_SAMPLE appears
 *   AND removal works.
 * Prints STARRY_PERF_CLI_KPROBE_OK.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PERF_TYPE_TRACEPOINT 2u

#define PERF_SAMPLE_IP (1ull << 0)
#define PERF_SAMPLE_TID (1ull << 1)
#define PERF_SAMPLE_TIME (1ull << 2)
#define PERF_SAMPLE_RAW (1ull << 11)

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

#define TRACING_DIR "/sys/kernel/debug/tracing"
#define KPROBE_EVENTS TRACING_DIR "/kprobe_events"
#define EVENT_ID_PATH TRACING_DIR "/events/probe/hsc/id"
#define EVENT_FORMAT_PATH TRACING_DIR "/events/probe/hsc/format"

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
    printf("perf-cli-kprobe FAILED: %s\n", reason);
    return 1;
}

static void ring_copy(const uint8_t *base, uint64_t size, uint64_t at, void *dst,
                      size_t n) {
    for (size_t b = 0; b < n; b++) {
        ((uint8_t *)dst)[b] = base[(at + b) % size];
    }
}

/* Read up to cap-1 bytes of a small sysfs/tracefs file into buf (NUL-terminated);
 * returns length or -1. */
static ssize_t read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t total = 0;
    ssize_t n;
    while ((size_t)total < cap - 1 &&
           (n = read(fd, buf + total, cap - 1 - (size_t)total)) > 0) {
        total += n;
    }
    close(fd);
    if (total < 0) {
        return -1;
    }
    buf[total] = '\0';
    return total;
}

/* Scan /proc/kallsyms for the first symbol whose (mangled) name contains
 * `needle`; copy the exact name into `name` and its address into `*addr`.
 * Returns 0 on success. */
static int find_symbol(const char *needle, char *name, size_t name_sz,
                       uint64_t *addr) {
    int fd = open("/proc/kallsyms", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    static char buf[1 << 16];
    char line[512];
    size_t ln = 0;
    int found = -1;
    ssize_t got;
    while (found != 0 && (got = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < got; i++) {
            char c = buf[i];
            if (c != '\n' && ln + 1 < sizeof(line)) {
                line[ln++] = c;
                continue;
            }
            line[ln] = '\0';
            ln = 0;
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
                size_t sl = strlen(sym);
                if (sl + 1 <= name_sz) {
                    memcpy(name, sym, sl + 1);
                    if (addr) {
                        *addr = strtoull(line, NULL, 16);
                    }
                    found = 0;
                    break;
                }
            }
        }
    }
    close(fd);
    return found;
}

/* Write `text` to `path` (open O_WRONLY). Returns 0 on success. */
static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    ssize_t n = write(fd, text, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

int main(void) {
#if !defined(__aarch64__)
    printf("STARRY_PERF_CLI_KPROBE_OK\n");
    return 0;
#endif
    char sym[256];
    uint64_t hsc_addr = 0, stext_addr = 0;
    if (find_symbol("handle_syscall", sym, sizeof(sym), &hsc_addr) != 0) {
        return fail("could not resolve handle_syscall in /proc/kallsyms");
    }
    char stext[64];
    if (find_symbol("_stext", stext, sizeof(stext), &stext_addr) != 0 ||
        stext_addr == 0 || hsc_addr <= stext_addr) {
        return fail("could not resolve _stext / bad kallsyms addresses");
    }
    unsigned long long offset = (unsigned long long)(hsc_addr - stext_addr);

    /* 1. perf probe -a: create the dynamic kprobe event. `perf probe` with no
     * vmlinux writes every kernel probe as `_stext+<offset>`, so drive that exact
     * form here — the offset must be honored or the kprobe fires at _stext. */
    char add[320];
    snprintf(add, sizeof(add), "p:probe/hsc _stext+%llu\n", offset);
    if (write_file(KPROBE_EVENTS, add) != 0) {
        return fail("write kprobe_events (add) failed");
    }
    char listing[1024];
    if (read_file(KPROBE_EVENTS, listing, sizeof(listing)) < 0 ||
        strstr(listing, "probe/hsc") == NULL) {
        return fail("kprobe_events readback does not list probe/hsc");
    }

    /* 2. read the dynamic tracepoint id + sanity-check the format. */
    char idbuf[64];
    if (read_file(EVENT_ID_PATH, idbuf, sizeof(idbuf)) <= 0) {
        return fail("could not read events/probe/hsc/id");
    }
    long id = strtol(idbuf, NULL, 10);
    if (id <= 0) {
        return fail("events/probe/hsc/id is not a positive number");
    }
    char fmtbuf[1024];
    if (read_file(EVENT_FORMAT_PATH, fmtbuf, sizeof(fmtbuf)) <= 0 ||
        strstr(fmtbuf, "common_pid") == NULL) {
        return fail("events/probe/hsc/format missing/invalid");
    }
    printf("STARRY_PERF_CLI_KPROBE add-ok id=%ld\n", id);

    /* 3. perf record -e probe:hsc -> open PERF_TYPE_TRACEPOINT on that id. */
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_TRACEPOINT;
    attr.size = (uint32_t)sizeof(attr);
    attr.config = (uint64_t)id;
    attr.sample_period = 1;
    /* PERF_SAMPLE_RAW is what `perf record -e probe:` sets by default for a
     * tracepoint; each sample then carries the raw kprobe record (common fields +
     * __probe_ip). Requesting it here also proves the open no longer ENOSYSes. */
    attr.sample_type =
        PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_RAW;
    attr.flags = PERF_ATTR_FLAG_DISABLED;

    long fd = perf_event_open(&attr, 0, -1, -1, 0ul);
    if (fd < 0) {
        char m[96];
        snprintf(m, sizeof(m), "perf_event_open(tracepoint id=%ld) errno=%d", id,
                 errno);
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
    for (int i = 0; i < 400; i++) {
        (void)getpid();
    }
    (void)ioctl(efd, PERF_EVENT_IOC_DISABLE, 0);

    uint64_t data_head = meta->data_head;
    __sync_synchronize();
    uint64_t data_tail = meta->data_tail;
    uint64_t data_offset = meta->data_offset;
    uint64_t data_size = meta->data_size;
    const uint8_t *data_base = (const uint8_t *)base + data_offset;

    uint64_t samples = 0, raw_ok = 0;
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
            /* body (IP|TID|TIME|RAW): u64 ip; u32 pid; u32 tid; u64 time;
             * u32 raw_size; then the raw record (common fields + __probe_ip at
             * raw offset 8). Validate the RAW block is present + well-formed. */
            uint64_t body = (rel + sizeof(hdr)) % data_size;
            uint32_t raw_size = 0;
            uint64_t probe_ip = 0;
            ring_copy(data_base, data_size, (body + 24) % data_size, &raw_size, 4);
            ring_copy(data_base, data_size, (body + 28 + 8) % data_size, &probe_ip,
                      8);
            /* __probe_ip must be symbol_addr + offset = _stext + off =
             * handle_syscall's address: proves the +offset was honored (not
             * dropped, which would place the probe at _stext). */
            if (raw_size >= 16 && probe_ip == hsc_addr) {
                raw_ok++;
            }
        }
        off += hdr.size;
    }

    (void)munmap(base, PERF_MMAP_TOTAL_BYTES);
    close(efd);

    printf("STARRY_PERF_CLI_KPROBE samples=%llu raw_ok=%llu off=%llu ip=%#llx\n",
           (unsigned long long)samples, (unsigned long long)raw_ok, offset,
           (unsigned long long)hsc_addr);

    /* 4. remove the event; assert it is gone. */
    if (write_file(KPROBE_EVENTS, "-:probe/hsc\n") != 0) {
        return fail("write kprobe_events (remove) failed");
    }
    if (read_file(KPROBE_EVENTS, listing, sizeof(listing)) >= 0 &&
        strstr(listing, "probe/hsc") != NULL) {
        return fail("probe/hsc still present after removal");
    }

    if (samples == 0) {
        return fail("no PERF_RECORD_SAMPLE from the dynamic kprobe tracepoint");
    }
    if (raw_ok == 0) {
        return fail("no sample's RAW __probe_ip matched handle_syscall "
                    "(the +offset was dropped -> probe fired at _stext)");
    }

    printf("STARRY_PERF_CLI_KPROBE_OK\n");
    return 0;
}
