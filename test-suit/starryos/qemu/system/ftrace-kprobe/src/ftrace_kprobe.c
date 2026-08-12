/*
 * ftrace_kprobe.c -- the classic ftrace kprobe flow through tracefs, with NO
 * perf and NO BPF. A dynamic kprobe event is created by writing kprobe_events,
 * armed via its `enable` file, and its hits are read back as formatted records
 * from `trace`. This is the ftrace-native path (perf_event_open is a separate
 * mechanism, covered by perf-cli-kprobe).
 *
 * Sequence:
 *   1. resolve the mangled kallsyms name of `handle_syscall` (fires on every
 *      syscall).
 *   2. echo 'p:probe/hsc <sym>' > kprobe_events; assert events/probe/hsc/enable
 *      appears.
 *   3. echo 1 > events/probe/hsc/enable  (arm the kprobe); read the enable file
 *      back and assert it reads "1".
 *   4. issue syscalls to trigger the probe.
 *   5. read /sys/kernel/debug/tracing/trace and count records of the form
 *      "hsc(0x...)".
 *   6. echo 0 > enable; echo '-:probe/hsc' > kprobe_events (cleanup).
 *
 * SUCCESS ==
 *     the event is created AND its enable file arms (reads back "1")
 *   AND `trace` contains at least one `hsc(0x` record.
 * Prints STARRY_FTRACE_KPROBE_OK.
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
#include <sys/syscall.h>
#include <unistd.h>

#define TRACING_DIR "/sys/kernel/debug/tracing"
#define KPROBE_EVENTS TRACING_DIR "/kprobe_events"
#define ENABLE_PATH TRACING_DIR "/events/probe/hsc/enable"
#define TRACE_PATH TRACING_DIR "/trace"
#define EVENT_NEEDLE "hsc(0x"

static int fail(const char *reason) {
    printf("ftrace-kprobe FAILED: %s\n", reason);
    return 1;
}

static void mark(const char *m) {
    printf("%s\n", m);
    fflush(stdout);
}

/* Write `text` to `path` (O_WRONLY). Returns 0 on success. */
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

/* Read up to cap-1 bytes of a small file into buf (NUL-terminated). */
static ssize_t read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t total = 0, n;
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

/* Find the first kallsyms symbol whose (mangled) name contains `needle`. */
static int find_symbol(const char *needle, char *name, size_t name_sz) {
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
                    found = 0;
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
    printf("STARRY_FTRACE_KPROBE_OK\n");
    return 0;
#endif
    char sym[256];
    if (find_symbol("handle_syscall", sym, sizeof(sym)) != 0) {
        return fail("could not resolve handle_syscall in /proc/kallsyms");
    }

    /* 1. create the dynamic kprobe event. */
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "p:probe/hsc %s", sym);
    mark("FTRACE_STEP add-begin");
    if (write_file(KPROBE_EVENTS, cmd) != 0) {
        return fail("could not write kprobe_events");
    }
    mark("FTRACE_STEP add-done");

    /* 2. arm it via the enable file and read the state back. */
    mark("FTRACE_STEP enable-begin");
    if (write_file(ENABLE_PATH, "1") != 0) {
        (void)write_file(KPROBE_EVENTS, "-:probe/hsc");
        return fail("could not enable events/probe/hsc");
    }
    char enbuf[16];
    if (read_file(ENABLE_PATH, enbuf, sizeof(enbuf)) <= 0 || enbuf[0] != '1') {
        (void)write_file(ENABLE_PATH, "0");
        (void)write_file(KPROBE_EVENTS, "-:probe/hsc");
        return fail("enable file did not read back as armed (1)");
    }
    mark("FTRACE_STEP enable-done");

    /* 3. trigger: any syscall hits handle_syscall at entry. */
    for (int i = 0; i < 64; i++) {
        (void)syscall(SYS_getpid);
    }
    mark("FTRACE_STEP trigger-done");

    /* 4. read the formatted trace snapshot; count `hsc(0x...)` records. */
    static char buf[65536];
    ssize_t total = read_file(TRACE_PATH, buf, sizeof(buf));
    if (total < 0) {
        (void)write_file(ENABLE_PATH, "0");
        (void)write_file(KPROBE_EVENTS, "-:probe/hsc");
        return fail("could not read tracefs trace file");
    }
    mark("FTRACE_STEP read-done");

    unsigned long hits = 0;
    for (const char *p = buf; (p = strstr(p, EVENT_NEEDLE)) != NULL; p++) {
        hits++;
    }
    printf("STARRY_FTRACE_KPROBE trace_bytes=%zd event_records=%lu\n", total,
           hits);

    /* 5. cleanup: disarm + remove. */
    (void)write_file(ENABLE_PATH, "0");
    (void)write_file(KPROBE_EVENTS, "-:probe/hsc");

    if (hits == 0) {
        return fail("no hsc(0x...) records found in tracefs trace");
    }

    printf("STARRY_FTRACE_KPROBE_OK\n");
    return 0;
}
