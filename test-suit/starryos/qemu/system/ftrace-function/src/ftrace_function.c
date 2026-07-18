/*
 * ftrace_function.c -- the ftrace function tracer via tracefs.
 *
 * Only functional in an opt-in `STARRY_FUNCTION_TRACER=1` kernel build
 * (-Zpatchable-function-entry), which exposes
 * /sys/kernel/debug/tracing/{available_tracers,current_tracer,set_ftrace_filter}.
 * When those are absent (a normal build), the test is a no-op success.
 *
 * Sequence:
 *   1. resolve the mangled kallsyms name of `handle_syscall` (runs on every
 *      syscall) and write it to set_ftrace_filter; read it back.
 *   2. echo function > current_tracer  (self-patches the function's 2-NOP entry
 *      into `mov x9,x30 ; bl ftrace_caller`); read current_tracer back == function.
 *   3. issue syscalls (getpid) to hit the patched entry.
 *   4. read `trace` and count `handle_syscall(...)` records.
 *   5. echo nop > current_tracer (unpatch).
 *
 * SUCCESS ==
 *     the tracer files are absent (normal build) — no-op OK
 *   OR filter + current_tracer arm AND `trace` has >=1 handle_syscall record.
 * Prints STARRY_FTRACE_FUNCTION_OK.
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
#define AVAIL_PATH TRACING_DIR "/available_tracers"
#define CURRENT_PATH TRACING_DIR "/current_tracer"
#define FILTER_PATH TRACING_DIR "/set_ftrace_filter"
#define TRACE_PATH TRACING_DIR "/trace"
#define EVENT_NEEDLE "handle_syscall"

static int fail(const char *reason) {
    printf("ftrace-function FAILED: %s\n", reason);
    return 1;
}

static void mark(const char *m) {
    printf("%s\n", m);
    fflush(stdout);
}

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
    /* Absent tracer files == normal (non-instrumented) build: no-op success. */
    if (access(CURRENT_PATH, F_OK) != 0) {
        printf("ftrace-function: no current_tracer (non-instrumented build), skipping\n");
        printf("STARRY_FTRACE_FUNCTION_OK\n");
        return 0;
    }

    char avail[128];
    if (read_file(AVAIL_PATH, avail, sizeof(avail)) <= 0 ||
        strstr(avail, "function") == NULL) {
        return fail("available_tracers missing 'function'");
    }

    char sym[256];
    if (find_symbol("handle_syscall", sym, sizeof(sym)) != 0) {
        return fail("could not resolve handle_syscall in /proc/kallsyms");
    }

    /* 1. filter to that function. */
    mark("FTRACE_STEP filter-begin");
    if (write_file(FILTER_PATH, sym) != 0) {
        return fail("could not write set_ftrace_filter");
    }
    char fbuf[512];
    if (read_file(FILTER_PATH, fbuf, sizeof(fbuf)) <= 0 ||
        strstr(fbuf, EVENT_NEEDLE) == NULL) {
        return fail("set_ftrace_filter did not accept handle_syscall");
    }
    mark("FTRACE_STEP filter-done");

    /* 2. select the function tracer (self-patches the entry). */
    mark("FTRACE_STEP tracer-begin");
    if (write_file(CURRENT_PATH, "function") != 0) {
        return fail("could not write current_tracer=function");
    }
    char cbuf[64];
    if (read_file(CURRENT_PATH, cbuf, sizeof(cbuf)) <= 0 ||
        strncmp(cbuf, "function", 8) != 0) {
        (void)write_file(CURRENT_PATH, "nop");
        return fail("current_tracer did not read back as function");
    }
    mark("FTRACE_STEP tracer-armed");

    /* 3. trigger the patched entry. */
    for (int i = 0; i < 64; i++) {
        (void)syscall(SYS_getpid);
    }
    mark("FTRACE_STEP trigger-done");

    /* 4. read the trace snapshot; count handle_syscall records. */
    static char tbuf[131072];
    ssize_t total = read_file(TRACE_PATH, tbuf, sizeof(tbuf));
    if (total < 0) {
        (void)write_file(CURRENT_PATH, "nop");
        return fail("could not read tracefs trace file");
    }
    mark("FTRACE_STEP read-done");

    unsigned long hits = 0;
    for (const char *p = tbuf; (p = strstr(p, EVENT_NEEDLE)) != NULL; p++) {
        hits++;
    }
    printf("STARRY_FTRACE_FUNCTION trace_bytes=%zd func_records=%lu\n", total,
           hits);

    /* 5. unpatch. */
    (void)write_file(CURRENT_PATH, "nop");

    if (hits == 0) {
        return fail("no handle_syscall records in tracefs trace");
    }

    /* Trace-all (empty `set_ftrace_filter` + current_tracer=function) is
     * implemented (batch-patched, alloc-free ring, try_lock push) but is NOT
     * exercised here: instrumenting every kernel function is a ~100x global
     * slowdown that, compounded with QEMU-TCG emulation, cannot complete a
     * workload in the harness timeout. Filtered tracing above is the practical,
     * validated mode (as it is on Linux). */

    printf("STARRY_FTRACE_FUNCTION_OK\n");
    return 0;
}
