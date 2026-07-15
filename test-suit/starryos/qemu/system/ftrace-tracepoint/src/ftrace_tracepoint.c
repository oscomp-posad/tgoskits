/*
 * ftrace_tracepoint.c -- ftrace static-tracepoint tracing through tracefs.
 *
 * The Linux ftrace way to trace a static tracepoint (no perf, no BPF): enable it
 * by writing its `enable` file, run the workload, then read the formatted trace
 * from `trace` (a snapshot) or `trace_pipe` (a blocking stream). StarryOS mounts
 * a tracefs at /sys/kernel/debug/tracing with events/<sys>/<event>/enable, trace,
 * and trace_pipe; an enabled tracepoint pushes a cooked record into the trace
 * ring buffer that `trace` / `trace_pipe` drain.
 *
 * This exercises that path end-to-end:
 *   1. echo 1 > events/syscalls/sys_enter_openat/enable
 *   2. issue openat() syscalls (each hits sys_enter_openat)
 *   3. read /sys/kernel/debug/tracing/trace and confirm sys_enter_openat records
 *      are present (a formatted "<comm>-<pid> [cpu] <ts>: sys_enter_openat: ..."
 *      line)
 *   4. echo 0 > .../enable
 *
 * It is also the regression test for the trace-to-pipe deadlock: the tracepoint
 * fire path pushes into the ring with preemption disabled, and used to take a
 * sleeping mutex there -> "sleep in atomic context" -> the CPU wedged on the
 * FIRST openat after enabling (the run hung to the timeout). The `FTRACE_STEP`
 * markers below bracket that exact spot, so a regression shows as output stopping
 * after `enable-done`.
 *
 * `trace` is used (not `trace_pipe`) because it is a non-blocking snapshot; the
 * blocking `trace_pipe` would hang once its records are drained.
 *
 * Scope note: full ftrace *function* tracing (current_tracer=function) is out of
 * scope -- it needs -pg / mcount / patchable-function-entry instrumentation the
 * kernel is not built with, and there is no current_tracer/available_tracers
 * node. Static-tracepoint tracing is the achievable ftrace surface here.
 *
 * SUCCESS ==
 *     the tracepoint enable file is writable
 *   AND after triggering, `trace` contains at least one "sys_enter_openat" record.
 * Prints STARRY_FTRACE_TRACEPOINT_OK.
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
#define ENABLE_PATH TRACING_DIR "/events/syscalls/sys_enter_openat/enable"
#define TRACE_PATH TRACING_DIR "/trace"
#define EVENT_NEEDLE "sys_enter_openat"

static int fail(const char *reason) {
    printf("ftrace-tracepoint FAILED: %s\n", reason);
    return 1;
}

/* Write a single-byte value ("0"/"1") to a tracefs control file. */
static int write_ctl(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n == (ssize_t)strlen(val) ? 0 : -1;
}

static void mark(const char *m) {
    printf("%s\n", m);
    fflush(stdout);
}

int main(void) {
    /* tracefs is arch-independent, so this runs on every arch. */
    mark("FTRACE_STEP enable-begin");
    if (write_ctl(ENABLE_PATH, "1") != 0) {
        return fail("could not enable sys_enter_openat via tracefs");
    }
    mark("FTRACE_STEP enable-done");

    /* Trigger the tracepoint: each openat() hits sys_enter_openat at entry. */
    for (int i = 0; i < 64; i++) {
        int t = (int)syscall(SYS_openat, AT_FDCWD, "/nonexistent-ftrace-probe",
                             O_RDONLY, 0);
        if (t >= 0) {
            close(t);
        }
    }
    mark("FTRACE_STEP trigger-done");

    /* Read the formatted trace snapshot and look for our event's records. */
    int fd = open(TRACE_PATH, O_RDONLY);
    if (fd < 0) {
        (void)write_ctl(ENABLE_PATH, "0");
        return fail("could not open tracefs trace file");
    }
    mark("FTRACE_STEP trace-opened");
    static char buf[65536];
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(buf) - 1 &&
           (n = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    mark("FTRACE_STEP read-done");

    /* Count how many records mention the event. */
    unsigned long hits = 0;
    for (const char *p = buf; (p = strstr(p, EVENT_NEEDLE)) != NULL; p++) {
        hits++;
    }

    printf("STARRY_FTRACE_TRACEPOINT trace_bytes=%zu event_records=%lu\n", total,
           hits);

    /* Disable again (best-effort cleanup). */
    (void)write_ctl(ENABLE_PATH, "0");

    if (hits == 0) {
        return fail("no sys_enter_openat records found in tracefs trace");
    }

    printf("STARRY_FTRACE_TRACEPOINT_OK\n");
    return 0;
}
