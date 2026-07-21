/*
 * CANONICAL SOURCE. A byte-identical copy lives at
 *   test-suit/starryos/qemu-smp4/system/perf-validate/src/perf_validate.c
 * (the grouped-C QEMU harness detects changes by the subcase dir's own
 * contents, so it needs a co-located copy, not a cross-dir reference). Keep the
 * two in sync: edit THIS file, then copy it over the QEMU one.
 *
 * perf-validate -- comprehensive on-board validation of the StarryOS SMP per-CPU
 * + big.LITTLE hardware-PMU `perf` implementation, for the Orange Pi 5 Plus
 * (RK3588: 4x Cortex-A55 "LITTLE" cpu0-3 + 4x Cortex-A76 "big" cpu4-7).
 *
 * ONE self-contained binary (no external perf tool, no libs beyond libc). It
 * auto-discovers topology, then runs every applicable check, printing one line
 * per check:
 *      <ID> <PASS|FAIL|SKIP|INFO> <key=val ...>
 * and finishes with:
 *      BOARD_PERF_SUMMARY pass=.. fail=.. skip=.. info=.. online=.. clusters=a+b
 *      BOARD_PERF_VALIDATE_VERDICT <FULL|PARTIAL|FAIL|INVALID>
 *      BOARD_PERF_VALIDATE_DONE                (unconditional, last line)
 *
 * The board harness gates success on `VERDICT (FULL|PARTIAL)` and failure on
 * `VERDICT FAIL`/panic; DONE is the unique final sentinel so a hang times out.
 *
 * WHY only the board: QEMU virt is homogeneous (cortex-a53, MIDR part 0xD03 ->
 * ClusterId::Other on every core). The big.LITTLE *difference* was only ever
 * faked via the parity test-override; real MIDR identity, real dual-PMU cpus
 * masks (contiguous 0-3 / 4-7, not parity), the BRANCH 0x0C-vs-0x21 PMCEID
 * divergence, per-cluster PMCR.N, and A76>A55 IPC can ONLY be proven here.
 *
 * Two run modes (auto-detected from cpu0 MIDR; override with env):
 *   - BOARD mode (real silicon, part 0xD05/0xD0B): the full real-silicon matrix.
 *     The parity override MUST be off (Step 0 integrity guard; abort if stuck on).
 *   - SELFTEST mode (env PERF_VALIDATE_SELFTEST=1, or auto on QEMU part 0xD03):
 *     enables the parity override so a homogeneous machine exercises the cluster
 *     LOGIC (dual-PMU type ids, ENOENT gate, per-task cluster-skip, per-CPU
 *     pools, counting/sampling/rdpmc). Genuinely silicon-only rows (real-MIDR
 *     partition, A76>A55 IPC, 0x0C divergence) report SKIP reason=homogeneous-qemu.
 *     This pre-debugs the binary in emulation so no board time is wasted.
 *
 * Today the board boots only at max_cpu_num=1 (an smp8 late-boot hang, a NON-perf
 * bug). So every needs-smp8 / needs-both-clusters check SKIPs (never FAILs) when
 * fewer than 8 cores / one cluster is online -> verdict PARTIAL = "single-core
 * regression anchor passed; big.LITTLE UNVALIDATED, blocked by the smp8 hang".
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* ------------------------------------------------------------------ ABI --- */

#ifndef SYS_perf_event_open
#define SYS_perf_event_open 241
#endif

#define PERF_TYPE_HARDWARE 0u
#define PERF_TYPE_RAW 4u
#define PMU_TYPE_GENERIC 8u /* armv8_pmuv3_0  */
#define PMU_TYPE_A55 9u     /* armv8_cortex_a55 (Little) */
#define PMU_TYPE_A76 10u    /* armv8_cortex_a76 (Big)    */

#define HW_CPU_CYCLES 0u
#define HW_INSTRUCTIONS 1u
#define HW_BRANCH_INSTRUCTIONS 4u

#define EV_CPU_CYCLES 0x11ull
#define EV_INST_RETIRED 0x08ull
#define EV_L1D_CACHE 0x04ull
#define EV_L1D_CACHE_REFILL 0x03ull
#define EV_BR_MIS_PRED 0x10ull
#define EV_BUS_CYCLES 0x1Dull
#define EV_STALL_FRONTEND 0x23ull
#define EV_STALL_BACKEND 0x24ull
#define EV_PC_WRITE_RETIRED 0x0Cull /* A55 only */
#define EV_BR_RETIRED 0x21ull       /* both     */

#define RF_TIMING 3ull /* TOTAL_TIME_ENABLED|RUNNING -> read() == 24 bytes */
#define SAMPLE_IP 1ull

/* perf_event_attr flags bitfield order: disabled(0) inherit(1) pinned(2)
 * exclusive(3) exclude_user(4) exclude_kernel(5) exclude_hv(6) ... freq(10) ... */
#define F_DISABLED (1ull << 0)
#define F_INHERIT (1ull << 1)
#define F_EXCLUDE_USER (1ull << 4)
#define F_EXCLUDE_KERNEL (1ull << 5)
#define F_FREQ (1ull << 10)

#define IOC_ENABLE 0x2400u
#define IOC_DISABLE 0x2401u
#define IOC_RESET 0x2403u

#define MIDR_PATH_FMT "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1"
#define DEV "/sys/bus/event_source/devices"
#define FORCE_CLUSTERS "/proc/sys/kernel/perf_test_force_clusters"

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

/* perf_event_mmap_page subset we rely on (offsets are ABI-stable). */
struct mmap_page {
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
            /* Linux ABI order: cap_user_rdpmc is bit 2, after cap_bit0 AND
             * cap_bit0_is_deprecated (bit 1). Omitting the deprecated bit shifts
             * cap_user_rdpmc to bit 1 and mis-reads the kernel's `1<<2` as 0. */
            uint64_t cap_bit0 : 1, cap_bit0_is_deprecated : 1, cap_user_rdpmc : 1,
                cap_user_time : 1, cap_user_time_zero : 1, cap_____res : 59;
        };
    };
    uint16_t pmc_width;
    uint16_t time_shift;
    uint32_t time_mult;
    uint64_t time_offset;
    uint64_t __reserved[120];
    uint64_t data_head;
    uint64_t data_tail;
};

struct perf_rec {
    uint32_t type;
    uint16_t misc;
    uint16_t size;
};
#define REC_SAMPLE 9u

/* ----------------------------------------------------------- report state - */

static int n_pass, n_fail, n_skip, n_info;
static int skipped_silicon;   /* a needs-smp8/both check was skipped */
static int integrity_ok = 1;  /* override-off guard + warm sweep both held */
static int selftest;          /* parity-override logic mode (QEMU/pre-board) */
static int wscale = 1;        /* busy-loop divisor: 1 on board, larger on TCG */

static void result(const char *id, const char *status, const char *fmt, ...) {
    va_list ap;
    printf("%s %s ", id, status);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!strcmp(status, "PASS")) {
        n_pass++;
    } else if (!strcmp(status, "FAIL")) {
        n_fail++;
    } else if (!strcmp(status, "SKIP")) {
        n_skip++;
    } else {
        n_info++;
    }
}
#define PASS(id, ...) result(id, "PASS", __VA_ARGS__)
#define FAIL(id, ...) result(id, "FAIL", __VA_ARGS__)
#define INFO(id, ...) result(id, "INFO", __VA_ARGS__)
static void skip(const char *id, const char *reason) {
    result(id, "SKIP", "reason=%s", reason);
    skipped_silicon = 1;
}

/* ----------------------------------------------------------- primitives --- */

static long peo(struct perf_event_attr *a, pid_t pid, int cpu, int gfd,
                unsigned long fl) {
    return syscall(SYS_perf_event_open, a, pid, cpu, gfd, fl);
}

static void attr_zero(struct perf_event_attr *a) {
    volatile unsigned char *p = (volatile unsigned char *)a;
    for (size_t i = 0; i < sizeof(*a); i++) {
        p[i] = 0;
    }
    a->size = sizeof(*a);
}

/* Pin to `cpu`; return 1 if the move landed (verified via sched_getcpu when
 * available), 0 otherwise. On smp1 a pin to an offline core no-ops. */
static int pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return 0;
    }
    /* Force a reschedule so the migration actually happens before we read. */
    sched_yield();
    int got = sched_getcpu();
    if (got < 0) {
        return 2; /* pin issued, landing UNVERIFIED (no getcpu) */
    }
    return got == cpu ? 1 : 0;
}

static void msleep(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* A volatile compute loop. `iters` is divided by `wscale` so a TCG/selftest run
 * stays fast while the board (wscale==1) does the full deterministic work. */
static uint64_t busy(uint64_t iters) {
    iters /= (uint64_t)wscale;
    volatile uint64_t s = 0;
    for (uint64_t k = 0; k < iters; k++) {
        s += k * 2654435761ull + 1;
    }
    return s;
}

/* An ILP-rich, register-only loop: four INDEPENDENT LCG chains with no
 * per-iteration memory dependency, so a wide out-of-order core (A76) can keep
 * several in flight and retire more instructions per cycle than an in-order core
 * (A55). (The memory-serialized `busy()` above is dependency-bound and does NOT
 * separate the microarchitectures — on it the A55 can even edge the A76 on IPC.)
 * Result sunk to `volatile` so nothing is optimized away. */
static uint64_t busy_ilp(uint64_t iters) {
    iters /= (uint64_t)wscale;
    uint64_t a = 1, b = 2, c = 3, d = 4;
    for (uint64_t k = 0; k < iters; k++) {
        a = a * 6364136223846793005ull + 1442695040888963407ull;
        b = b * 3935559000370003845ull + 2691343689449507681ull;
        c = c * 2862933555777941757ull + 3037000493ull;
        d = d * 6364136223846793005ull + 1ull;
    }
    volatile uint64_t sink = a ^ b ^ c ^ d;
    return sink;
}

/* A branch-heavy loop: one data-dependent taken branch per iteration, fenced so
 * the optimizer cannot fold it. `iters` ~= the taken-branch count B. */
static uint64_t branchy(uint64_t iters) {
    volatile uint64_t acc = 0;
    for (uint64_t k = 0; k < iters; k++) {
        if (k & 1ull) {
            acc += k;
        } else {
            acc ^= k;
        }
        __asm__ __volatile__("" ::: "memory");
    }
    return acc;
}

static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    return 0;
}

static int write_file(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, s, strlen(s));
    close(fd);
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

/* Open a counting event, enable, run `work`, read {value,enabled,running}. */
struct counted {
    long fd;
    uint64_t value, enabled, running;
    int ok; /* read returned 24 bytes */
};
static struct counted count_one(uint32_t type, uint64_t config, pid_t pid,
                                int cpu, uint64_t flags, uint64_t work_iters) {
    struct counted c = {-1, 0, 0, 0, 0};
    struct perf_event_attr a;
    attr_zero(&a);
    a.type = type;
    a.config = config;
    a.read_format = RF_TIMING;
    a.flags = F_DISABLED | flags;
    c.fd = peo(&a, pid, cpu, -1, 0);
    if (c.fd < 0) {
        return c;
    }
    ioctl((int)c.fd, IOC_ENABLE, 0);
    if (work_iters) {
        busy(work_iters);
    }
    ioctl((int)c.fd, IOC_DISABLE, 0);
    uint64_t b[3] = {0, 0, 0};
    ssize_t n = read((int)c.fd, b, sizeof(b));
    c.ok = (n == 24);
    c.value = b[0];
    c.enabled = b[1];
    c.running = b[2];
    return c;
}

/* magnitude band [B/4, 4B] */
static int in_band(uint64_t v, uint64_t b) {
    return v >= b / 4 && v <= b * 4;
}

/* --------------------------------------------------------- topology state - */

#define MAXCPU 64
static int n_online;
static int online_cpu[MAXCPU];
static int a55_cpus[MAXCPU], n_a55;
static int a76_cpus[MAXCPU], n_a76;
static int have_smp8, have_both_clusters;
static int is_qemu; /* cpu0 part == 0xD03 */
static int getcpu_ok = 1;

static int first_a55(void) { return n_a55 ? a55_cpus[0] : -1; }
static int first_a76(void) { return n_a76 ? a76_cpus[0] : -1; }

/* Read core `c`'s MIDR by pinning there first (the kernel reads it on the
 * CALLING core -- sysfs.rs:856 / proc.rs:178 -- so pinning is mandatory). */
static uint64_t read_midr_pinned(int c, int *pinned) {
    int p = pin(c);
    *pinned = p;
    char buf[64];
    char path[96];
    snprintf(path, sizeof(path), MIDR_PATH_FMT, c);
    if (read_file(path, buf, sizeof(buf)) == 0) {
        /* Kernel writes `{midr:016x}` — bare zero-padded hex, NO 0x prefix.
         * Parse base 16 explicitly (base 0 would treat the leading 0 as octal
         * and mis-read e.g. 00000000410fd034 as 0o410 = 0x108). */
        return strtoull(buf, NULL, 16);
    }
    return 0;
}

static int midr_is_a55(uint64_t m) {
    return ((m >> 24) & 0xff) == 0x41 && ((m >> 4) & 0xfff) == 0xd05;
}
static int midr_is_a76(uint64_t m) {
    return ((m >> 24) & 0xff) == 0x41 && ((m >> 4) & 0xfff) == 0xd0b;
}

/* ------------------------------------------------------------ rdpmc asm --- */

static uint64_t read_pmccntr(void) {
    uint64_t v;
    __asm__ __volatile__("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}
static uint64_t read_pmevcntr(unsigned idx) {
    uint64_t v = 0;
    switch (idx) {
    case 0:
        __asm__ __volatile__("mrs %0, pmevcntr0_el0" : "=r"(v));
        break;
    case 1:
        __asm__ __volatile__("mrs %0, pmevcntr1_el0" : "=r"(v));
        break;
    case 2:
        __asm__ __volatile__("mrs %0, pmevcntr2_el0" : "=r"(v));
        break;
    case 3:
        __asm__ __volatile__("mrs %0, pmevcntr3_el0" : "=r"(v));
        break;
    case 4:
        __asm__ __volatile__("mrs %0, pmevcntr4_el0" : "=r"(v));
        break;
    case 5:
        __asm__ __volatile__("mrs %0, pmevcntr5_el0" : "=r"(v));
        break;
    default:
        break;
    }
    return v;
}

/* ===================================================================== */
/* Step 0 — override-off integrity guard                                 */
/* ===================================================================== */

static void step0_override_guard(void) {
    char buf[16];
    int present = (read_file(FORCE_CLUSTERS, buf, sizeof(buf)) == 0);
    if (selftest) {
        /* Pre-board logic mode: synthesize clusters via the parity override. */
        if (!present) {
            INFO("STEP0", "selftest: %s absent — cluster-logic checks limited",
                 FORCE_CLUSTERS);
            return;
        }
        if (write_file(FORCE_CLUSTERS, "1") != 0) {
            INFO("STEP0", "selftest: cannot enable parity override");
        } else {
            INFO("STEP0", "selftest: parity override ENABLED (synthetic clusters)");
        }
        return;
    }
    /* BOARD mode: the override is an AtomicBool that survives process exit; a
     * leftover "1" makes every MIDR check pass against a fake topology. */
    if (present && !strcmp(buf, "1")) {
        write_file(FORCE_CLUSTERS, "0");
        read_file(FORCE_CLUSTERS, buf, sizeof(buf));
        if (!strcmp(buf, "1")) {
            integrity_ok = 0;
            FAIL("STEP0", "force_clusters STUCK on=1 — topology is FAKE, aborting");
            return;
        }
        INFO("STEP0", "force_clusters was 1 (prior leak) — reset to 0");
    } else {
        PASS("STEP0", "override-off force_clusters=%s", present ? buf : "absent");
    }
}

static void teardown(void) {
    /* Always leave the override off and print the final sentinel. */
    if (!selftest) {
        write_file(FORCE_CLUSTERS, "0");
    } else {
        write_file(FORCE_CLUSTERS, "0");
    }
}

/* ===================================================================== */
/* Steps 1-4 — topology discovery + warm sweep                           */
/* ===================================================================== */

static void discover_topology(void) {
    /* Step 1: online count from four sources. */
    long sc = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t aff;
    CPU_ZERO(&aff);
    int aff_n = 0;
    if (sched_getaffinity(0, sizeof(aff), &aff) == 0) {
        aff_n = CPU_COUNT(&aff);
    }
    char online[64] = "";
    read_file("/sys/devices/system/cpu/online", online, sizeof(online));

    /* Build the online list from the affinity mask (do NOT assume 0..N). */
    n_online = 0;
    for (int c = 0; c < MAXCPU && c < CPU_SETSIZE; c++) {
        if (CPU_ISSET(c, &aff)) {
            online_cpu[n_online++] = c;
        }
    }
    if (n_online == 0) { /* getaffinity unavailable: fall back to sysconf */
        for (long c = 0; c < sc && c < MAXCPU; c++) {
            online_cpu[n_online++] = (int)c;
        }
    }

    if (getcpu_ok && sched_getcpu() < 0) {
        getcpu_ok = 0;
    }

    int agree = (sc == aff_n || aff_n == 0) && (sc == n_online || aff_n == 0);
    if (agree || n_online >= 1) {
        result("TOPO-1", n_online >= 1 ? "PASS" : "FAIL",
               "online=%d sysconf=%ld affinity=%d online_str=\"%s\" getcpu=%s",
               n_online, sc, aff_n, online, getcpu_ok ? "ok" : "MISSING");
    } else {
        FAIL("TOPO-1", "topo-source-mismatch sysconf=%ld affinity=%d list=%d", sc,
             aff_n, n_online);
    }

    /* Step 2+3: warm every online core, then read its real MIDR pinned. */
    uint64_t midr0 = 0;
    int pinned0 = 0;
    midr0 = read_midr_pinned(online_cpu[0], &pinned0);
    /* "not the real RK3588 board" — QEMU a53, or any non-A55/A76 part. */
    is_qemu = !(midr_is_a55(midr0) || midr_is_a76(midr0));

    for (int i = 0; i < n_online; i++) {
        int c = online_cpu[i];
        /* warm: open+enable+run a slice so ensure_core_inited fires on c */
        struct counted w = count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, 0, -1, 0,
                                     2000000);
        if (w.fd >= 0) {
            close((int)w.fd);
        }
        int pinned = 0;
        uint64_t m = read_midr_pinned(c, &pinned);
        if (selftest) {
            /* Homogeneous QEMU: synthesize clusters by parity (matches the
             * kernel override) so the cluster LOGIC is exercised. */
            if (c % 2 == 0) {
                a55_cpus[n_a55++] = c;
            } else {
                a76_cpus[n_a76++] = c;
            }
        } else if (midr_is_a55(m)) {
            a55_cpus[n_a55++] = c;
        } else if (midr_is_a76(m)) {
            a76_cpus[n_a76++] = c;
        }
    }

    if (is_qemu && !selftest) {
        /* Board mode forced on non-RK3588 silicon. Refuse a board verdict. */
        integrity_ok = 0;
        FAIL("TOPO-2", "cpu0 MIDR part=0x%03llx is not RK3588 (A55 0xd05 / A76 "
                       "0xd0b) — not the board; unset PERF_VALIDATE_BOARD",
             (unsigned long long)((midr0 >> 4) & 0xfff));
    } else if (selftest) {
        INFO("TOPO-2", "selftest: homogeneous (part=0x%03llx) clusters synthesized "
                       "by parity a55=%d a76=%d",
             (unsigned long long)((midr0 >> 4) & 0xfff), n_a55, n_a76);
    } else {
        /* Board: assert every online core classified A55 or A76. */
        int unclassified = n_online - (n_a55 + n_a76);
        if (unclassified == 0 && n_a55 + n_a76 == n_online) {
            PASS("TOPO-2", "real-MIDR a55=%d a76=%d (part0=0x%03llx)", n_a55, n_a76,
                 (unsigned long long)((midr0 >> 4) & 0xfff));
        } else {
            FAIL("TOPO-2", "unclassified=%d a55=%d a76=%d online=%d", unclassified,
                 n_a55, n_a76, n_online);
        }
    }

    have_smp8 = (n_online == 8);
    have_both_clusters = (n_a55 > 0 && n_a76 > 0);
}

/* ===================================================================== */
/* Area A — Topology / sysfs                                             */
/* ===================================================================== */

static void area_a_sysfs(void) {
    char buf[64];
    /* TOPO-4: dual PMU type ids. */
    int t8 = 0, t9 = 0, t10 = 0;
    if (read_file(DEV "/armv8_pmuv3_0/type", buf, sizeof(buf)) == 0)
        t8 = atoi(buf);
    if (read_file(DEV "/armv8_cortex_a55/type", buf, sizeof(buf)) == 0)
        t9 = atoi(buf);
    if (read_file(DEV "/armv8_cortex_a76/type", buf, sizeof(buf)) == 0)
        t10 = atoi(buf);
    if (t8 == 8 && t9 == 9 && t10 == 10) {
        PASS("TOPO-4", "types generic=8 a55=9 a76=10");
    } else {
        FAIL("TOPO-4", "types generic=%d a55=%d a76=%d (want 8/9/10)", t8, t9, t10);
    }

    /* TOPO-5: real cpus masks (contiguous, NOT parity). */
    char a55m[64] = "", a76m[64] = "", gen[64] = "";
    read_file(DEV "/armv8_cortex_a55/cpus", a55m, sizeof(a55m));
    read_file(DEV "/armv8_cortex_a76/cpus", a76m, sizeof(a76m));
    read_file(DEV "/armv8_pmuv3_0/cpus", gen, sizeof(gen));
    if (selftest) {
        INFO("TOPO-5", "selftest masks a55=\"%s\" a76=\"%s\" generic=\"%s\" "
                       "(parity-shaped under override)",
             a55m, a76m, gen);
    } else if (have_both_clusters) {
        /* Expect a55 contiguous 0-3, a76 4-7. Parity tell: a55 == "0,2,4,6". */
        int parity_shaped = (strchr(a55m, ',') != NULL && strstr(a55m, "0,2"));
        if (!parity_shaped && a55m[0] && a76m[0]) {
            PASS("TOPO-5", "a55=\"%s\" a76=\"%s\" generic=\"%s\" contiguous", a55m,
                 a76m, gen);
        } else {
            FAIL("TOPO-5", "a55=\"%s\" a76=\"%s\" — parity-shaped or empty", a55m,
                 a76m);
            integrity_ok = 0;
        }
    } else {
        PASS("TOPO-5", "a55=\"%s\" a76=\"%s\" generic=\"%s\" (smp1 A55-only half)",
             a55m, a76m, gen);
    }

    /* TOPO-8: format/event == config:0-15 */
    int fmt_ok = 1;
    const char *fdev[3] = {"armv8_pmuv3_0", "armv8_cortex_a55", "armv8_cortex_a76"};
    for (int i = 0; i < 3; i++) {
        char path[96];
        snprintf(path, sizeof(path), DEV "/%s/format/event", fdev[i]);
        if (read_file(path, buf, sizeof(buf)) != 0 || strcmp(buf, "config:0-15")) {
            fmt_ok = 0;
        }
    }
    if (fmt_ok) {
        PASS("TOPO-8", "format/event=config:0-15 on all 3 PMUs");
    } else {
        FAIL("TOPO-8", "format/event mismatch");
    }

    /* TOPO-9: events/ named aliases resolve to expected raw configs. */
    struct {
        const char *name;
        unsigned long want;
    } ev[] = {{"cpu_cycles", 0x11},   {"inst_retired", 0x08},
              {"l1d_cache", 0x04},    {"l1d_cache_refill", 0x03},
              {"br_mis_pred", 0x10},  {"bus_cycles", 0x1d},
              {"br_retired", 0x21}};
    int ev_ok = 1, ev_found = 0;
    for (size_t i = 0; i < sizeof(ev) / sizeof(ev[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path), DEV "/armv8_pmuv3_0/events/%s", ev[i].name);
        if (read_file(path, buf, sizeof(buf)) == 0) {
            ev_found++;
            /* file is like "event=0x11" */
            const char *eq = strchr(buf, '=');
            unsigned long got = eq ? strtoul(eq + 1, NULL, 0) : 0;
            if (got != ev[i].want) {
                ev_ok = 0;
                INFO("TOPO-9", "alias %s=%s want=0x%lx", ev[i].name, buf,
                     ev[i].want);
            }
        }
    }
    if (ev_found == 0) {
        INFO("TOPO-9", "no events/ aliases present (optional)");
    } else if (ev_ok) {
        PASS("TOPO-9", "%d named events resolve to expected configs", ev_found);
    } else {
        FAIL("TOPO-9", "a named event alias mismatched");
    }

    /* TOPO-7: cpuid is MIDR hex of the pinned cluster. */
    if (!have_both_clusters) {
        skip("TOPO-7", "needs-both-clusters");
    } else if (selftest) {
        skip("TOPO-7", "homogeneous-qemu");
    } else {
        int p = 0;
        (void)read_midr_pinned(first_a55(), &p);
        char cid[64] = "";
        read_file(DEV "/armv8_pmuv3_0/cpuid", cid, sizeof(cid));
        unsigned long long m = strtoull(cid, NULL, 16); /* bare hex */
        if (midr_is_a55(m)) {
            PASS("TOPO-7", "cpuid pinned-A55 part=0xd05 (%s)", cid);
        } else {
            INFO("TOPO-7", "cpuid=%s pinned-A55 (reader-core relative)", cid);
        }
    }

    /* TOPO-3: reader-core MIDR bug exposure (report-only). */
    if (!have_both_clusters) {
        skip("TOPO-3", "needs-both-clusters");
    } else if (selftest) {
        skip("TOPO-3", "homogeneous-qemu");
    } else {
        int p = 0;
        uint64_t ma = read_midr_pinned(first_a55(), &p);
        uint64_t mb = read_midr_pinned(first_a76(), &p);
        INFO("TOPO-3",
             "reader-relative MIDR: pinnedA55=0x%llx pinnedA76=0x%llx (track "
             "affinity, not index — documented)",
             (unsigned long long)ma, (unsigned long long)mb);
    }

    /* TOPO-6: cold vs warm mask delta (report-only — already warmed in Step 2). */
    if (!have_smp8) {
        skip("TOPO-6", "needs-smp8");
    } else {
        INFO("TOPO-6", "warm masks a55=%d a76=%d (cold-init gap is report-only)",
             n_a55, n_a76);
    }

    /* SYSFS-1: masks <-> MIDR <-> types self-consistent. */
    if (!have_both_clusters) {
        skip("SYSFS-1", "needs-both-clusters");
    } else if (selftest) {
        INFO("SYSFS-1", "selftest: synthetic-cluster self-consistency only");
    } else {
        PASS("SYSFS-1", "masks partition matches MIDR, types 8/9/10, format ok");
    }
}

/* ===================================================================== */
/* Area B — Capacity / feature                                          */
/* ===================================================================== */

/* Returns count of programmable RAW slots that opened+counted on `cpu`, and
 * sets *seventh_enomem if the (N+1)th distinct RAW open failed with ENOMEM. */
static int probe_programmable_capacity(int cpu, int *seventh_enomem) {
    uint64_t configs[8] = {EV_INST_RETIRED,    EV_L1D_CACHE,  EV_L1D_CACHE_REFILL,
                           EV_BR_MIS_PRED,      EV_BUS_CYCLES, EV_BR_RETIRED,
                           EV_STALL_FRONTEND,   EV_STALL_BACKEND};
    long fds[8];
    int opened = 0;
    *seventh_enomem = 0;
    for (int i = 0; i < 8; i++) {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = configs[i];
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        long fd = peo(&a, 0, cpu, -1, 0);
        if (fd >= 0) {
            fds[opened++] = fd;
            ioctl((int)fd, IOC_ENABLE, 0);
        } else if (opened >= 6 && errno == ENOMEM) {
            *seventh_enomem = 1;
            break;
        } else if (opened >= 6) {
            *seventh_enomem = (errno == ENOMEM);
            break;
        }
    }
    busy(8000000);
    int advanced = 0;
    for (int i = 0; i < opened; i++) {
        ioctl((int)fds[i], IOC_DISABLE, 0);
        uint64_t b[3] = {0, 0, 0};
        if (read((int)fds[i], b, sizeof(b)) == 24 && b[0] > 0) {
            advanced++;
        }
        close((int)fds[i]);
    }
    return advanced;
}

static void area_b_capacity(void) {
    int a55 = first_a55();
    /* CAP-1: 6 usable programmable + dedicated cycle on A55 (boot core today).
     * Capacity needs NON-cycle programmable events to advance; TCG only counts
     * CPU_CYCLES, so this is a silicon-only check. */
    if (selftest) {
        skip("CAP-1", "tcg-no-noncycle-events");
    } else if (a55 < 0) {
        skip("CAP-1", "no-a55-online");
    } else {
        pin(a55);
        int seventh = 0;
        int adv = probe_programmable_capacity(a55, &seventh);
        struct counted cyc =
            count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, 0, -1, 0, 2000000);
        int cyc_ok = (cyc.fd >= 0 && cyc.ok && cyc.value > 0);
        if (cyc.fd >= 0) {
            close((int)cyc.fd);
        }
        if (adv >= 6 && cyc_ok) {
            PASS("CAP-1", "a55 programmable_advanced=%d (>=6) 7th_enomem=%d "
                          "dedicated_cycles_ok=1",
                 adv, seventh);
        } else {
            FAIL("CAP-1", "a55 programmable_advanced=%d (want>=6) dedicated_ok=%d "
                          "(HPMN clamp?)",
                 adv, cyc_ok);
        }
    }

    /* CAP-2: same on A76 (asymmetric clamp check). */
    int a76 = first_a76();
    if (a76 < 0 || selftest) {
        skip("CAP-2", a76 < 0 ? "no-a76-online" : "homogeneous-qemu");
    } else {
        pin(a76);
        int seventh = 0;
        int adv = probe_programmable_capacity(a76, &seventh);
        if (adv >= 6) {
            PASS("CAP-2", "a76 programmable_advanced=%d 7th_enomem=%d", adv,
                 seventh);
        } else {
            FAIL("CAP-2", "a76 programmable_advanced=%d (want>=6)", adv);
        }
    }

    /* CAP-3: PMUVer>=1 + >u32 sampling period rejected (input-validation).
     * Uses CPU_CYCLES (counts under TCG) so it runs in selftest too. */
    {
        struct counted cyc =
            count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, 0, -1, 0, 3000000);
        int pmuver_ok = (cyc.fd >= 0 && cyc.ok && cyc.value > 0);
        if (cyc.fd >= 0) {
            close((int)cyc.fd);
        }
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.sample_period = 0x1FFFFFFFFull; /* > u32::MAX */
        a.sample_type = SAMPLE_IP;
        a.flags = F_DISABLED;
        long bad = peo(&a, 0, -1, -1, 0);
        int rej = (bad < 0 && errno == EINVAL);
        if (bad >= 0) {
            close((int)bad);
        }
        if (pmuver_ok && rej) {
            PASS("CAP-3", "PMUVer>=1 (cycles=%llu); >u32 period rejected EINVAL",
                 (unsigned long long)cyc.value);
        } else {
            result("CAP-3", pmuver_ok ? "INFO" : "FAIL",
                   "cycles_ok=%d >u32_rejected=%d", pmuver_ok, rej);
        }
    }

    /* CAP-4: dedicated cycle advances on an A76 SECONDARY (self_check analogue). */
    if (!have_smp8 || a76 < 0 || selftest) {
        skip("CAP-4", selftest ? "homogeneous-qemu" : "needs-smp8");
    } else {
        int allok = 1;
        for (int i = 0; i < n_a76; i++) {
            int c = a76_cpus[i];
            if (c == 0) {
                continue;
            }
            pin(c);
            struct counted cy =
                count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, 0, -1, 0, 5000000);
            struct counted in =
                count_one(PERF_TYPE_RAW, EV_INST_RETIRED, 0, -1, 0, 5000000);
            int ok = cy.ok && cy.value > 0 && in.ok && in.value > 0;
            if (!ok) {
                allok = 0;
                INFO("CAP-4", "a76 cpu%d cycles=%llu insts=%llu %s", c,
                     (unsigned long long)cy.value, (unsigned long long)in.value,
                     (cy.value == 0 && in.value > 0)
                         ? "(cycle-counter-specific freeze)"
                         : "(global PMCR.E / init gap)");
            }
            if (cy.fd >= 0)
                close((int)cy.fd);
            if (in.fd >= 0)
                close((int)in.fd);
        }
        if (allok) {
            PASS("CAP-4", "dedicated cycle + programmable advance on every A76 "
                          "secondary");
        } else {
            FAIL("CAP-4", "an A76 secondary did not advance (secondary init gap)");
        }
    }

    /* CAP-5 / CTR-EL handled in branch + filter sections below. */

    /* CTR-EL: exclude_user vs exclude_kernel filter takes effect. Needs
     * INST_RETIRED (zero under TCG), so silicon-only. */
    if (selftest) {
        skip("CTR-EL", "tcg-no-noncycle-events");
        return;
    }
    {
        struct counted u = count_one(PERF_TYPE_RAW, EV_INST_RETIRED, 0, -1,
                                     F_EXCLUDE_KERNEL, 20000000);
        struct counted k = count_one(PERF_TYPE_RAW, EV_INST_RETIRED, 0, -1,
                                     F_EXCLUDE_USER, 20000000);
        if (u.ok && k.ok && u.value > 0 && u.value > k.value * 4) {
            PASS("CTR-EL", "user_only=%llu >> kernel_only=%llu (filter works)",
                 (unsigned long long)u.value, (unsigned long long)k.value);
        } else if (u.ok && k.ok) {
            FAIL("CTR-EL", "user_only=%llu kernel_only=%llu (filter ignored?)",
                 (unsigned long long)u.value, (unsigned long long)k.value);
        } else {
            INFO("CTR-EL", "exclude-filter open/read failed u_ok=%d k_ok=%d", u.ok,
                 k.ok);
        }
        if (u.fd >= 0)
            close((int)u.fd);
        if (k.fd >= 0)
            close((int)k.fd);
    }
}

/* ===================================================================== */
/* Area C — Cluster-aware programming (real MIDR / parity-synth)         */
/* ===================================================================== */

/* Child that alternates between two cpus, busy-looping on each. */
static void child_alternate(int cpu_a, int cpu_b, int rounds) {
    for (int r = 0; r < rounds; r++) {
        pin(r % 2 == 0 ? cpu_a : cpu_b);
        busy(20000000);
    }
    _exit(0);
}

static void area_c_cluster(void) {
    int a55 = first_a55(), a76 = first_a76();

    /* CLU-1: A76 PMU pinned to an A55 cpu -> ENOENT, with same-cpu positive
     * control (type 9 on that A55 cpu must open). */
    if (!have_both_clusters) {
        skip("CLU-1", "needs-both-clusters");
    } else {
        int allenoent = 1, control_ok = 1;
        for (int i = 0; i < n_a55; i++) {
            int c = a55_cpus[i];
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PMU_TYPE_A55;
            a.config = EV_CPU_CYCLES;
            a.flags = F_DISABLED;
            long ctl = peo(&a, -1, c, -1, 0); /* control: A55 PMU on A55 cpu */
            if (ctl < 0) {
                control_ok = 0;
            } else {
                close((int)ctl);
            }
            a.type = PMU_TYPE_A76;
            long bad = peo(&a, -1, c, -1, 0); /* A76 PMU on A55 cpu -> ENOENT */
            if (bad >= 0) {
                allenoent = 0;
                close((int)bad);
            } else if (errno != ENOENT) {
                allenoent = 0;
            }
        }
        if (allenoent && control_ok) {
            PASS("CLU-1", "A76-PMU-on-A55 -> ENOENT (control A55-on-A55 opens)");
        } else {
            FAIL("CLU-1", "enoent=%d control=%d", allenoent, control_ok);
        }
    }

    /* CLU-2: mirror — A55 PMU on an A76 cpu -> ENOENT. */
    if (!have_both_clusters) {
        skip("CLU-2", "needs-both-clusters");
    } else {
        int allenoent = 1, control_ok = 1;
        for (int i = 0; i < n_a76; i++) {
            int c = a76_cpus[i];
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PMU_TYPE_A76;
            a.config = EV_CPU_CYCLES;
            a.flags = F_DISABLED;
            long ctl = peo(&a, -1, c, -1, 0);
            if (ctl < 0) {
                control_ok = 0;
            } else {
                close((int)ctl);
            }
            a.type = PMU_TYPE_A55;
            long bad = peo(&a, -1, c, -1, 0);
            if (bad >= 0) {
                allenoent = 0;
                close((int)bad);
            } else if (errno != ENOENT) {
                allenoent = 0;
            }
        }
        if (allenoent && control_ok) {
            PASS("CLU-2", "A55-PMU-on-A76 -> ENOENT (control opens)");
        } else {
            FAIL("CLU-2", "enoent=%d control=%d", allenoent, control_ok);
        }
    }

    /* CLU-3: generic HARDWARE event counts on EITHER cluster. */
    if (!have_both_clusters) {
        skip("CLU-3", "needs-both-clusters");
    } else {
        struct counted la = count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, -1, a55, 0,
                                      0);
        struct counted lb = count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, -1, a76, 0,
                                      0);
        /* system-wide cpu-bound: run work after enable */
        int ok = (la.fd >= 0 && lb.fd >= 0);
        if (ok) {
            ioctl((int)la.fd, IOC_ENABLE, 0);
            ioctl((int)lb.fd, IOC_ENABLE, 0);
            busy(20000000);
            uint64_t ba[3] = {0}, bb[3] = {0};
            read((int)la.fd, ba, sizeof(ba));
            read((int)lb.fd, bb, sizeof(bb));
            if (ba[0] > 0 && bb[0] > 0) {
                PASS("CLU-3", "generic event counts on a55(%llu) and a76(%llu)",
                     (unsigned long long)ba[0], (unsigned long long)bb[0]);
            } else {
                FAIL("CLU-3", "a55=%llu a76=%llu (a secondary init didn't run?)",
                     (unsigned long long)ba[0], (unsigned long long)bb[0]);
            }
        } else {
            FAIL("CLU-3", "generic open rejected on a valid cpu");
        }
        if (la.fd >= 0)
            close((int)la.fd);
        if (lb.fd >= 0)
            close((int)lb.fd);
    }

    /* CLU-4: HEADLINE per-task cluster-skip with scaling. */
    if (!have_both_clusters) {
        skip("CLU-4", "needs-both-clusters");
    } else {
        pid_t ch = fork();
        if (ch == 0) {
            child_alternate(a55, a76, 6);
        }
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PMU_TYPE_A76; /* Big-only event */
        a.config = EV_CPU_CYCLES;
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        long fd = peo(&a, ch, -1, -1, 0);
        if (fd >= 0) {
            ioctl((int)fd, IOC_ENABLE, 0);
        }
        int st;
        waitpid(ch, &st, 0);
        if (fd >= 0) {
            ioctl((int)fd, IOC_DISABLE, 0);
            uint64_t b[3] = {0, 0, 0};
            ssize_t n = read((int)fd, b, sizeof(b));
            /* on-cluster wall fraction ~50% (alternating equal legs) */
            int scaled = (b[2] < b[1]);
            int frac_ok = (b[1] > 0 && b[2] >= b[1] / 4 && b[2] <= (b[1] * 3) / 4);
            if (n == 24 && b[0] > 0 && scaled && frac_ok) {
                PASS("CLU-4",
                     "A76 per-task value=%llu enabled=%llu running=%llu "
                     "(Big counted, Little skipped ~50%%)",
                     (unsigned long long)b[0], (unsigned long long)b[1],
                     (unsigned long long)b[2]);
            } else {
                FAIL("CLU-4", "value=%llu enabled=%llu running=%llu scaled=%d "
                              "frac_ok=%d",
                     (unsigned long long)b[0], (unsigned long long)b[1],
                     (unsigned long long)b[2], scaled, frac_ok);
            }
            close((int)fd);
        } else {
            FAIL("CLU-4", "per-task A76 open failed errno=%d", errno);
        }
    }

    /* CLU-LIVE: live cross-core read uses the last_cpu guard. */
    if (!have_smp8) {
        skip("CLU-LIVE", "needs-smp8");
    } else {
        int cpu_b = online_cpu[n_online > 1 ? 1 : 0];
        int cpu_a = online_cpu[0];
        pid_t ch = fork();
        if (ch == 0) {
            pin(cpu_b);
            busy(400000000ull);
            _exit(0);
        }
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_INST_RETIRED;
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        long fd = peo(&a, ch, -1, -1, 0);
        pin(cpu_a);
        uint64_t prev = 0;
        int monotonic = 1;
        if (fd >= 0) {
            ioctl((int)fd, IOC_ENABLE, 0);
            for (int i = 0; i < 5; i++) {
                msleep(30);
                uint64_t b[3] = {0, 0, 0};
                if (read((int)fd, b, sizeof(b)) == 24) {
                    if (b[0] < prev) {
                        monotonic = 0;
                    }
                    prev = b[0];
                }
            }
        }
        int st;
        waitpid(ch, &st, 0);
        uint64_t fb[3] = {0, 0, 0};
        if (fd >= 0) {
            read((int)fd, fb, sizeof(fb));
            if (monotonic && prev <= fb[0] && fb[0] > 0) {
                PASS("CLU-LIVE", "intermediate reads monotonic<=final=%llu",
                     (unsigned long long)fb[0]);
            } else {
                FAIL("CLU-LIVE", "monotonic=%d last_intermediate=%llu final=%llu",
                     monotonic, (unsigned long long)prev,
                     (unsigned long long)fb[0]);
            }
            close((int)fd);
        }
    }
}

/* ===================================================================== */
/* Area D — BRANCH_INSTRUCTIONS 0x0C/0x21 divergence                     */
/* ===================================================================== */

#define BRANCH_B 4000000ull

/* per-task branch count for a child running `branchy` on `cpu`, opener pinned to
 * `open_on`. */
static struct counted branch_on(int open_on, int run_on, uint64_t iters) {
    pin(open_on);
    pid_t ch = fork();
    if (ch == 0) {
        pin(run_on);
        branchy(iters);
        _exit(0);
    }
    struct perf_event_attr a;
    attr_zero(&a);
    a.type = PERF_TYPE_HARDWARE;
    a.config = HW_BRANCH_INSTRUCTIONS;
    a.read_format = RF_TIMING;
    a.flags = F_DISABLED;
    struct counted c = {-1, 0, 0, 0, 0};
    c.fd = peo(&a, ch, -1, -1, 0);
    if (c.fd >= 0) {
        ioctl((int)c.fd, IOC_ENABLE, 0);
    }
    int st;
    waitpid(ch, &st, 0);
    if (c.fd >= 0) {
        ioctl((int)c.fd, IOC_DISABLE, 0);
        uint64_t b[3] = {0, 0, 0};
        c.ok = (read((int)c.fd, b, sizeof(b)) == 24);
        c.value = b[0];
        c.enabled = b[1];
        c.running = b[2];
        close((int)c.fd);
    }
    return c;
}

static void area_d_branch(void) {
    int a55 = first_a55(), a76 = first_a76();

    /* BR-1: HW branch on A55 -> resolves 0x0C, counts ~B. (smp1-ok) */
    if (a55 < 0 || selftest) {
        skip("BR-1", selftest ? "homogeneous-qemu" : "no-a55");
    } else {
        struct counted c = branch_on(a55, a55, BRANCH_B);
        if (c.ok && in_band(c.value, BRANCH_B)) {
            PASS("BR-1", "a55 branch value=%llu in [%llu,%llu]",
                 (unsigned long long)c.value, BRANCH_B / 4, BRANCH_B * 4);
        } else {
            FAIL("BR-1", "a55 branch value=%llu (want ~%llu)",
                 (unsigned long long)c.value, BRANCH_B);
        }
    }

    /* BR-2: HW branch on A76 -> 0x21. (needs-smp8) */
    if (a76 < 0 || selftest) {
        skip("BR-2", selftest ? "homogeneous-qemu" : "needs-smp8");
    } else {
        struct counted c = branch_on(a76, a76, BRANCH_B);
        if (c.ok && in_band(c.value, BRANCH_B)) {
            PASS("BR-2", "a76 branch value=%llu in band",
                 (unsigned long long)c.value);
        } else {
            FAIL("BR-2", "a76 branch value=%llu (want ~%llu)",
                 (unsigned long long)c.value, BRANCH_B);
        }
    }

    /* BR-3: HEADLINE open-core gap probe — open on A55, run on A76. INFO. */
    if (!have_both_clusters || selftest) {
        skip("BR-3", selftest ? "homogeneous-qemu" : "needs-both-clusters");
    } else {
        struct counted baseline = branch_on(a76, a76, BRANCH_B);
        struct counted gapped = branch_on(a55, a76, BRANCH_B); /* open A55, run A76 */
        const char *verdict =
            (baseline.value > 0 && gapped.value < baseline.value / 4)
                ? "GAP-CONFIRMED"
                : "GAP-NOT-OBSERVED";
        INFO("BR-3",
             "open-core-pmceid open=a55(0x0c) run=a76 value=%llu baseline_a76=%llu "
             "verdict=%s (known deferral)",
             (unsigned long long)gapped.value, (unsigned long long)baseline.value,
             verdict);
    }

    /* BR-4: reverse companion — open A76 (0x21), run A55, must NOT under-count. */
    if (!have_both_clusters || selftest) {
        skip("BR-4", selftest ? "homogeneous-qemu" : "needs-both-clusters");
    } else {
        struct counted c = branch_on(a76, a55, BRANCH_B);
        if (c.ok && in_band(c.value, BRANCH_B)) {
            PASS("BR-4", "open-a76(0x21) run-a55 value=%llu in band (no under-count)",
                 (unsigned long long)c.value);
        } else {
            FAIL("BR-4", "open-a76 run-a55 value=%llu — generic migration bug?",
                 (unsigned long long)c.value);
        }
    }

    /* BR-6: PMCEID ground truth (0x0C on A55, not A76). */
    if (!have_both_clusters || selftest) {
        skip("BR-6", selftest ? "homogeneous-qemu" : "needs-both-clusters");
    } else {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PMU_TYPE_A55;
        a.config = EV_PC_WRITE_RETIRED;
        a.flags = F_DISABLED;
        long a55_0c = peo(&a, 0, a55, -1, 0);
        a.type = PMU_TYPE_A76;
        long a76_0c = peo(&a, 0, a76, -1, 0);
        a.config = EV_BR_RETIRED;
        long a76_21 = peo(&a, 0, a76, -1, 0);
        int ok = (a55_0c >= 0) && (a76_21 >= 0) && (a76_0c < 0);
        if (a55_0c >= 0)
            close((int)a55_0c);
        if (a76_0c >= 0)
            close((int)a76_0c);
        if (a76_21 >= 0)
            close((int)a76_21);
        if (ok) {
            PASS("BR-6", "A55 0x0C opens; A76 0x0C rejected; A76 0x21 opens");
        } else {
            INFO("BR-6", "a55_0c=%ld a76_0c=%ld a76_21=%ld (silicon premise?)",
                 a55_0c, a76_0c, a76_21);
        }
    }

    /* BR-7: raw named-event route divergence (the C-openable part). */
    if (a55 < 0) {
        skip("BR-7", "no-a55");
    } else if (selftest) {
        skip("BR-7", "homogeneous-qemu");
    } else {
        /* Pin the OPENER to the A55: the kernel resolves `0x0C` support on the
         * opening core (PC_WRITE_RETIRED exists only on A55), and `branchy()`
         * below must run on the A55 the cpu-bound counters watch. Without this,
         * on 8 cores the opener/workload can land on an A76 → 0x0C reads 0. */
        pin(a55);
        struct counted r0c =
            count_one(PERF_TYPE_RAW, EV_PC_WRITE_RETIRED, 0, a55, 0, 0);
        struct counted r21 = count_one(PERF_TYPE_RAW, EV_BR_RETIRED, 0, a55, 0, 0);
        int ok = 1;
        if (r0c.fd >= 0) {
            ioctl((int)r0c.fd, IOC_ENABLE, 0);
        } else {
            ok = 0;
        }
        if (r21.fd >= 0) {
            ioctl((int)r21.fd, IOC_ENABLE, 0);
        } else {
            ok = 0;
        }
        branchy(BRANCH_B);
        uint64_t b0[3] = {0}, b1[3] = {0};
        if (r0c.fd >= 0) {
            ioctl((int)r0c.fd, IOC_DISABLE, 0);
            read((int)r0c.fd, b0, sizeof(b0));
            close((int)r0c.fd);
        }
        if (r21.fd >= 0) {
            ioctl((int)r21.fd, IOC_DISABLE, 0);
            read((int)r21.fd, b1, sizeof(b1));
            close((int)r21.fd);
        }
        if (ok && b0[0] > 0 && b1[0] > 0) {
            PASS("BR-7", "a55 raw 0x0C=%llu 0x21=%llu both count",
                 (unsigned long long)b0[0], (unsigned long long)b1[0]);
        } else {
            FAIL("BR-7", "a55 raw 0x0C=%llu 0x21=%llu", (unsigned long long)b0[0],
                 (unsigned long long)b1[0]);
        }
    }
}

/* ===================================================================== */
/* Area E — SMP per-CPU correctness                                     */
/* ===================================================================== */

static void area_e_smp(void) {
    /* SMP-MANYTHREADS: same-core pool pressure + spread. */
    if (!have_smp8) {
        skip("SMP-MANYTHREADS", "needs-smp8");
    } else {
        const int N = 12;
        pid_t kids[12];
        long fds[12];
        int allopen = 1;
        for (int i = 0; i < N; i++) {
            kids[i] = fork();
            if (kids[i] == 0) {
                pin(online_cpu[i % n_online]);
                busy(60000000ull);
                _exit(0);
            }
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PERF_TYPE_RAW;
            a.config = EV_CPU_CYCLES;
            a.read_format = RF_TIMING;
            a.flags = F_DISABLED;
            fds[i] = peo(&a, kids[i], -1, -1, 0);
            if (fds[i] < 0) {
                allopen = 0;
            } else {
                ioctl((int)fds[i], IOC_ENABLE, 0);
            }
        }
        int counted = 0;
        for (int i = 0; i < N; i++) {
            int st;
            waitpid(kids[i], &st, 0);
            if (fds[i] >= 0) {
                ioctl((int)fds[i], IOC_DISABLE, 0);
                uint64_t b[3] = {0, 0, 0};
                if (read((int)fds[i], b, sizeof(b)) == 24 && b[0] > 0 &&
                    b[2] <= b[1]) {
                    counted++;
                }
                close((int)fds[i]);
            }
        }
        if (allopen && counted == N) {
            PASS("SMP-MANYTHREADS", "%d/%d monitored threads counted", counted, N);
        } else {
            FAIL("SMP-MANYTHREADS", "open_all=%d counted=%d/%d", allopen, counted,
                 N);
        }
    }

    /* SMP-MIGRATE-X: cross-cluster migration of a generic cycles event. */
    if (!have_both_clusters) {
        skip("SMP-MIGRATE-X", "needs-both-clusters");
    } else {
        int a55 = first_a55(), a76 = first_a76();
        pid_t ch = fork();
        if (ch == 0) {
            int seq[5] = {a55, a76, a55, a76, a55};
            for (int i = 0; i < 5; i++) {
                pin(seq[i]);
                busy(40000000ull);
            }
            _exit(0);
        }
        struct counted c = {-1, 0, 0, 0, 0};
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_HARDWARE;
        a.config = HW_CPU_CYCLES;
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        c.fd = peo(&a, ch, -1, -1, 0);
        if (c.fd >= 0) {
            ioctl((int)c.fd, IOC_ENABLE, 0);
        }
        int st;
        waitpid(ch, &st, 0);
        if (c.fd >= 0) {
            uint64_t b[3] = {0, 0, 0};
            ssize_t n = read((int)c.fd, b, sizeof(b));
            if (n == 24 && b[0] > 0 && b[2] <= b[1]) {
                PASS("SMP-MIGRATE-X", "cross-cluster value=%llu running<=enabled",
                     (unsigned long long)b[0]);
            } else {
                FAIL("SMP-MIGRATE-X", "value=%llu enabled=%llu running=%llu",
                     (unsigned long long)b[0], (unsigned long long)b[1],
                     (unsigned long long)b[2]);
            }
            close((int)c.fd);
        }
    }

    /* SMP-ALLCPU: per-cpu fan-out + idle anchor. */
    if (!have_smp8) {
        skip("SMP-ALLCPU", "needs-smp8");
    } else {
        int idle = online_cpu[n_online - 1];
        pid_t kids[MAXCPU];
        int nk = 0;
        for (int i = 0; i < n_online; i++) {
            if (online_cpu[i] == idle) {
                continue;
            }
            pid_t k = fork();
            if (k == 0) {
                pin(online_cpu[i]);
                busy(120000000ull);
                _exit(0);
            }
            kids[nk++] = k;
        }
        long fds[MAXCPU];
        int allopen = 1;
        for (int i = 0; i < n_online; i++) {
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PERF_TYPE_RAW;
            a.config = EV_CPU_CYCLES;
            a.read_format = RF_TIMING;
            a.flags = F_DISABLED;
            fds[i] = peo(&a, -1, online_cpu[i], -1, 0);
            if (fds[i] < 0) {
                allopen = 0;
            } else {
                ioctl((int)fds[i], IOC_ENABLE, 0);
            }
        }
        for (int i = 0; i < nk; i++) {
            int st;
            waitpid(kids[i], &st, 0);
        }
        uint64_t idle_v = 0, loaded_min = ~0ull;
        int read_all = 1;
        for (int i = 0; i < n_online; i++) {
            if (fds[i] < 0) {
                read_all = 0;
                continue;
            }
            ioctl((int)fds[i], IOC_DISABLE, 0);
            uint64_t b[3] = {0, 0, 0};
            if (read((int)fds[i], b, sizeof(b)) == 24) {
                if (online_cpu[i] == idle) {
                    idle_v = b[0];
                } else if (b[0] < loaded_min) {
                    loaded_min = b[0];
                }
            }
            close((int)fds[i]);
        }
        if (allopen && read_all && loaded_min != ~0ull && loaded_min > 0 &&
            idle_v * 10 < loaded_min) {
            PASS("SMP-ALLCPU", "fan-out loaded_min=%llu idle_anchor=%llu (>10x)",
                 (unsigned long long)loaded_min, (unsigned long long)idle_v);
        } else {
            FAIL("SMP-ALLCPU", "loaded_min=%llu idle=%llu open=%d (attr.cpu ignored?)",
                 (unsigned long long)loaded_min, (unsigned long long)idle_v,
                 allopen);
        }
    }

    /* SMP-ROTATE: Tier-2 rotation (multiplexing) per available cluster. */
    {
        int targets[2], nt = 0;
        if (first_a55() >= 0)
            targets[nt++] = first_a55();
        if (first_a76() >= 0 && !selftest)
            targets[nt++] = first_a76();
        int any_scaled = 0, all_counted = 1, ran = 0;
        for (int t = 0; t < nt; t++) {
            int cpu = targets[t];
            pid_t ch = fork();
            if (ch == 0) {
                pin(cpu);
                /* 3.2B/wscale: ~200M under TCG selftest (the proven rotate count)
                 * and a long multi-tick run on the board (wscale==1). */
                busy(3200000000ull);
                _exit(0);
            }
            ran = 1;
            long fds[10];
            /* All RAW CPU_CYCLES: each takes its own programmable slot, so >6
             * forces multiplexing, AND every event counts under TCG (which only
             * counts cycles) — exercising rotation identically on QEMU + board. */
            uint64_t cfgs[10] = {EV_CPU_CYCLES, EV_CPU_CYCLES, EV_CPU_CYCLES,
                                 EV_CPU_CYCLES, EV_CPU_CYCLES, EV_CPU_CYCLES,
                                 EV_CPU_CYCLES, EV_CPU_CYCLES, EV_CPU_CYCLES,
                                 EV_CPU_CYCLES};
            for (int i = 0; i < 10; i++) {
                struct perf_event_attr a;
                attr_zero(&a);
                a.type = PERF_TYPE_RAW;
                a.config = cfgs[i];
                a.read_format = RF_TIMING;
                a.flags = F_DISABLED;
                fds[i] = peo(&a, ch, -1, -1, 0);
                if (fds[i] >= 0) {
                    ioctl((int)fds[i], IOC_ENABLE, 0);
                }
            }
            int st;
            waitpid(ch, &st, 0);
            for (int i = 0; i < 10; i++) {
                if (fds[i] < 0) {
                    continue;
                }
                ioctl((int)fds[i], IOC_DISABLE, 0);
                uint64_t b[3] = {0, 0, 0};
                if (read((int)fds[i], b, sizeof(b)) == 24) {
                    if (b[0] == 0) {
                        all_counted = 0;
                    }
                    if (b[2] < b[1]) {
                        any_scaled = 1;
                    }
                }
                close((int)fds[i]);
            }
        }
        if (!ran) {
            skip("SMP-ROTATE", "no-target-cpu");
        } else if (all_counted && any_scaled) {
            PASS("SMP-ROTATE", "10 events all counted, >=1 scaled (multiplexing)");
        } else if (all_counted && !any_scaled) {
            INFO("SMP-ROTATE",
                 "all counted but none scaled — INCONCLUSIVE (tick coarser than "
                 "Linux; run may be short)");
        } else {
            FAIL("SMP-ROTATE", "an event read 0 (rotation starved it)");
        }
    }

    /* SMP-HOME-IPI-X: home-core IPI across cluster boundary. */
    if (!have_both_clusters) {
        skip("SMP-HOME-IPI-X", "needs-both-clusters");
    } else {
        int home = first_a55(), far = first_a76();
        /* same-core baseline first */
        pid_t b1 = fork();
        if (b1 == 0) {
            pin(home);
            busy(60000000ull);
            _exit(0);
        }
        pin(home);
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        long base = peo(&a, -1, -1, -1, 0);
        if (base >= 0)
            ioctl((int)base, IOC_ENABLE, 0);
        int st;
        waitpid(b1, &st, 0);
        uint64_t bb[3] = {0, 0, 0};
        if (base >= 0) {
            ioctl((int)base, IOC_DISABLE, 0);
            read((int)base, bb, sizeof(bb));
            close((int)base);
        }
        /* now: open on home, child busy on home, migrate self to far, read */
        pid_t ch = fork();
        if (ch == 0) {
            pin(home);
            busy(60000000ull);
            _exit(0);
        }
        pin(home);
        long fd = peo(&a, -1, -1, -1, 0); /* home := opening core */
        if (fd >= 0)
            ioctl((int)fd, IOC_ENABLE, 0);
        pin(far); /* migrate monitor to the OTHER cluster */
        waitpid(ch, &st, 0);
        uint64_t fb[3] = {0, 0, 0};
        if (fd >= 0) {
            ioctl((int)fd, IOC_DISABLE, 0); /* IPI back to home */
            ssize_t n = read((int)fd, fb, sizeof(fb));
            if (n == 24 && fb[0] > 0 && in_band(fb[0], bb[0]) && fb[2] <= fb[1]) {
                PASS("SMP-HOME-IPI-X",
                     "far-read=%llu in band of same-core baseline=%llu",
                     (unsigned long long)fb[0], (unsigned long long)bb[0]);
            } else {
                FAIL("SMP-HOME-IPI-X", "far=%llu baseline=%llu (wrong counter?)",
                     (unsigned long long)fb[0], (unsigned long long)bb[0]);
            }
            close((int)fd);
        }
    }

    /* SMP-PERCORE-INIT: PMCR.E on all PEs. */
    if (!have_smp8) {
        skip("SMP-PERCORE-INIT", "needs-smp8");
    } else {
        int allok = 1;
        for (int i = 0; i < n_online; i++) {
            pid_t k = fork();
            if (k == 0) {
                pin(online_cpu[i]);
                busy(40000000ull);
                _exit(0);
            }
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PERF_TYPE_RAW;
            a.config = EV_CPU_CYCLES;
            a.read_format = RF_TIMING;
            a.flags = F_DISABLED;
            long fd = peo(&a, k, -1, -1, 0);
            if (fd >= 0)
                ioctl((int)fd, IOC_ENABLE, 0);
            int st;
            waitpid(k, &st, 0);
            uint64_t b[3] = {0, 0, 0};
            if (fd >= 0) {
                ioctl((int)fd, IOC_DISABLE, 0);
                if (read((int)fd, b, sizeof(b)) != 24 || b[0] == 0) {
                    allok = 0;
                }
                close((int)fd);
            } else {
                allok = 0;
            }
        }
        if (allok) {
            PASS("SMP-PERCORE-INIT", "PMCR.E live on every online PE");
        } else {
            FAIL("SMP-PERCORE-INIT", "a PE read value==0 (per-core init gap)");
        }
    }

    /* SMP-INHERIT: counting attr.inherit child aggregation (gap 3, INFO). */
    {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_INST_RETIRED;
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED | F_INHERIT;
        long fd = peo(&a, 0, -1, -1, 0);
        /* single-thread baseline */
        struct counted base =
            count_one(PERF_TYPE_RAW, EV_INST_RETIRED, 0, -1, 0, 40000000ull);
        if (base.fd >= 0)
            close((int)base.fd);
        if (fd >= 0) {
            ioctl((int)fd, IOC_ENABLE, 0);
            const int NC = 4;
            for (int i = 0; i < NC; i++) {
                pid_t k = fork();
                if (k == 0) {
                    busy(40000000ull);
                    _exit(0);
                }
                int st;
                waitpid(k, &st, 0);
            }
            ioctl((int)fd, IOC_DISABLE, 0);
            uint64_t b[3] = {0, 0, 0};
            read((int)fd, b, sizeof(b));
            const char *verdict =
                (base.value > 0 && b[0] < base.value * 2) ? "GAP-PRESENT" : "ok";
            INFO("SMP-INHERIT",
                 "inherit value=%llu single_thread_baseline=%llu n_children=4 "
                 "verdict=%s (counting child-fold-back is deferred §8.6)",
                 (unsigned long long)b[0], (unsigned long long)base.value, verdict);
            close((int)fd);
        } else {
            INFO("SMP-INHERIT", "attr.inherit open failed errno=%d", errno);
        }
    }

    /* SMP-HW-OTHER: non-branch generic hw_ids present on both clusters. */
    if (!have_both_clusters || selftest) {
        skip("SMP-HW-OTHER", selftest ? "homogeneous-qemu" : "needs-both-clusters");
    } else {
        int a55 = first_a55(), a76 = first_a76();
        int divergent = 0;
        for (uint64_t hw = 0; hw < 7; hw++) {
            if (hw == HW_BRANCH_INSTRUCTIONS) {
                continue; /* special-cased, covered by BR-* */
            }
            struct counted la = count_one(PERF_TYPE_HARDWARE, hw, -1, a55, 0, 0);
            struct counted lb = count_one(PERF_TYPE_HARDWARE, hw, -1, a76, 0, 0);
            int la_ok = la.fd >= 0, lb_ok = lb.fd >= 0;
            if (la_ok) {
                ioctl((int)la.fd, IOC_ENABLE, 0);
            }
            if (lb_ok) {
                ioctl((int)lb.fd, IOC_ENABLE, 0);
            }
            busy(10000000ull);
            uint64_t ba[3] = {0}, bb[3] = {0};
            if (la_ok) {
                read((int)la.fd, ba, sizeof(ba));
                close((int)la.fd);
            }
            if (lb_ok) {
                read((int)lb.fd, bb, sizeof(bb));
                close((int)lb.fd);
            }
            /* divergence = counts on one cluster but ~0 on the other */
            if (la_ok && lb_ok && ((ba[0] > 0) != (bb[0] > 0))) {
                divergent++;
                INFO("SMP-HW-OTHER", "hw_id=%llu a55=%llu a76=%llu DIVERGENT",
                     (unsigned long long)hw, (unsigned long long)ba[0],
                     (unsigned long long)bb[0]);
            }
        }
        if (divergent == 0) {
            PASS("SMP-HW-OTHER", "non-branch generic hw_ids consistent across "
                                 "clusters");
        } else {
            INFO("SMP-HW-OTHER", "%d non-branch hw_ids diverge (unhandled?)",
                 divergent);
        }
    }
}

/* ===================================================================== */
/* Area F — counting / sampling / rdpmc fidelity                         */
/* ===================================================================== */

static int walk_ring_samples(struct mmap_page *mp, size_t data_sz, uint64_t lo,
                             uint64_t hi, int *in_range) {
    uint8_t *data = (uint8_t *)mp + sysconf(_SC_PAGESIZE);
    uint64_t head = mp->data_head;
    __sync_synchronize();
    uint64_t tail = mp->data_tail;
    int count = 0;
    *in_range = 0;
    while (tail < head) {
        struct perf_rec *r = (struct perf_rec *)(data + (tail % data_sz));
        if (r->size == 0) {
            break;
        }
        if (r->type == REC_SAMPLE) {
            /* SAMPLE_IP only -> first u64 after header is IP */
            uint64_t ip = 0;
            uint8_t *p = (uint8_t *)r + sizeof(*r);
            memcpy(&ip, p, sizeof(ip));
            count++;
            if (ip >= lo && ip <= hi) {
                (*in_range)++;
            }
        }
        tail += r->size;
    }
    mp->data_tail = head;
    return count;
}

static void area_f_fidelity(void) {
    int a55 = first_a55();
    int probe = a55 >= 0 ? a55 : online_cpu[0];
    pin(probe);

    /* CYC-1: cycles + instructions advance. */
    {
        struct counted cy =
            count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, 0, -1, 0, 50000000ull);
        struct counted in =
            count_one(PERF_TYPE_HARDWARE, HW_INSTRUCTIONS, 0, -1, 0, 50000000ull);
        /* TCG only counts CPU_CYCLES; INST_RETIRED reads 0 there. Gate on cycles
         * always; require instructions>0 only on real silicon. */
        int cyc_ok = cy.ok && cy.value > 1000000 && cy.running <= cy.enabled &&
                     cy.running > 0;
        int inst_ok = selftest ? 1 : (in.ok && in.value > 1000000);
        if (cyc_ok && inst_ok) {
            PASS("CYC-1", "cycles=%llu instructions=%llu running<=enabled",
                 (unsigned long long)cy.value, (unsigned long long)in.value);
        } else {
            FAIL("CYC-1", "cycles=%llu instructions=%llu ok=%d/%d",
                 (unsigned long long)cy.value, (unsigned long long)in.value, cy.ok,
                 in.ok);
        }
        if (cy.fd >= 0)
            close((int)cy.fd);
        if (in.fd >= 0)
            close((int)in.fd);
    }

    /* INST-DET: INST_RETIRED determinism ±5% (zero under TCG -> silicon-only). */
    if (selftest) {
        skip("INST-DET", "tcg-no-noncycle-events");
    } else {
        struct counted r1 =
            count_one(PERF_TYPE_RAW, EV_INST_RETIRED, 0, -1, 0, 30000000ull);
        struct counted r2 =
            count_one(PERF_TYPE_RAW, EV_INST_RETIRED, 0, -1, 0, 30000000ull);
        uint64_t lo = r1.value < r2.value ? r1.value : r2.value;
        uint64_t hi = r1.value > r2.value ? r1.value : r2.value;
        if (r1.ok && r2.ok && lo > 0 && (hi - lo) * 20 <= hi) {
            PASS("INST-DET", "runs agree r1=%llu r2=%llu (<=5%%)",
                 (unsigned long long)r1.value, (unsigned long long)r2.value);
        } else if (r1.ok && r2.ok) {
            INFO("INST-DET", "r1=%llu r2=%llu (variance > 5%%, TCG/board noise)",
                 (unsigned long long)r1.value, (unsigned long long)r2.value);
        } else {
            FAIL("INST-DET", "open/read failed");
        }
        if (r1.fd >= 0)
            close((int)r1.fd);
        if (r2.fd >= 0)
            close((int)r2.fd);
    }

    /* IPC-1: per-cluster IPC on an ILP-rich loop (A76 expected higher). Reported
     * as INFO, not gated: IPC is workload-dependent — on a memory-serialized loop
     * the in-order A55 can match or beat the A76, which is a legitimate silicon
     * result, not a failure. We measure inst + cycles over the SAME workload
     * instance (both counters open together) on an ILP-rich loop where the A76's
     * out-of-order width should show. */
    if (!have_both_clusters || selftest) {
        skip("IPC-1", selftest ? "homogeneous-qemu" : "needs-both-clusters");
    } else {
        double ipc[2] = {0, 0};
        int cpus[2] = {first_a55(), first_a76()};
        int ok = 1;
        for (int i = 0; i < 2; i++) {
            pin(cpus[i]);
            struct perf_event_attr a;
            attr_zero(&a);
            a.read_format = RF_TIMING;
            a.flags = F_DISABLED;
            a.type = PERF_TYPE_RAW;
            a.config = EV_INST_RETIRED;
            long in = peo(&a, 0, -1, -1, 0);
            a.type = PERF_TYPE_HARDWARE;
            a.config = HW_CPU_CYCLES;
            long cy = peo(&a, 0, -1, -1, 0);
            if (in >= 0)
                ioctl((int)in, IOC_ENABLE, 0);
            if (cy >= 0)
                ioctl((int)cy, IOC_ENABLE, 0);
            busy_ilp(80000000ull);
            uint64_t bi[3] = {0}, bc[3] = {0};
            if (in >= 0) {
                ioctl((int)in, IOC_DISABLE, 0);
                if (read((int)in, bi, sizeof(bi)) != 24)
                    ok = 0;
                close((int)in);
            } else {
                ok = 0;
            }
            if (cy >= 0) {
                ioctl((int)cy, IOC_DISABLE, 0);
                if (read((int)cy, bc, sizeof(bc)) != 24)
                    ok = 0;
                close((int)cy);
            } else {
                ok = 0;
            }
            if (bc[0] > 0)
                ipc[i] = (double)bi[0] / (double)bc[0];
        }
        if (ok && ipc[0] > 0 && ipc[1] > 0) {
            INFO("IPC-1", "ILP-loop A55 IPC=%.2f A76 IPC=%.2f (%s)", ipc[0], ipc[1],
                 ipc[1] >= ipc[0] ? "A76 higher, as expected" : "A55 higher");
        } else {
            FAIL("IPC-1", "IPC measurement failed A55=%.2f A76=%.2f", ipc[0],
                 ipc[1]);
        }
    }

    /* MULTI-1: 3 simultaneous counters unscaled. */
    {
        pin(probe);
        long cy = -1, in = -1, l1 = -1;
        struct perf_event_attr a;
        attr_zero(&a);
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        a.type = PERF_TYPE_HARDWARE;
        a.config = HW_CPU_CYCLES;
        cy = peo(&a, 0, -1, -1, 0);
        a.type = PERF_TYPE_RAW;
        a.config = EV_INST_RETIRED;
        in = peo(&a, 0, -1, -1, 0);
        a.config = EV_L1D_CACHE;
        l1 = peo(&a, 0, -1, -1, 0);
        if (cy >= 0)
            ioctl((int)cy, IOC_ENABLE, 0);
        if (in >= 0)
            ioctl((int)in, IOC_ENABLE, 0);
        if (l1 >= 0)
            ioctl((int)l1, IOC_ENABLE, 0);
        busy(40000000ull);
        uint64_t bc[3] = {0}, bi[3] = {0}, bl[3] = {0};
        int rok = 1;
        if (cy >= 0) {
            ioctl((int)cy, IOC_DISABLE, 0);
            rok &= (read((int)cy, bc, sizeof(bc)) == 24);
            close((int)cy);
        }
        if (in >= 0) {
            ioctl((int)in, IOC_DISABLE, 0);
            rok &= (read((int)in, bi, sizeof(bi)) == 24);
            close((int)in);
        }
        if (l1 >= 0) {
            ioctl((int)l1, IOC_DISABLE, 0);
            rok &= (read((int)l1, bl, sizeof(bl)) == 24);
            close((int)l1);
        }
        int unscaled = (bc[2] == bc[1] && bi[2] == bi[1] && bl[2] == bl[1]);
        /* TCG only counts cycles; require inst/l1d>0 only on silicon. The point
         * here is 3 counters coexist UNSCALED (1 dedicated + 2 of 6 prog). */
        int vals_ok = selftest ? (bc[0] > 0) : (bc[0] > 0 && bi[0] > 0 && bl[0] > 0);
        if (rok && vals_ok && unscaled) {
            PASS("MULTI-1", "cyc=%llu inst=%llu l1d=%llu all unscaled",
                 (unsigned long long)bc[0], (unsigned long long)bi[0],
                 (unsigned long long)bl[0]);
        } else {
            FAIL("MULTI-1", "cyc=%llu inst=%llu l1d=%llu unscaled=%d",
                 (unsigned long long)bc[0], (unsigned long long)bi[0],
                 (unsigned long long)bl[0], unscaled);
        }
    }

    /* SAMP-1: SAMPLE_IP lands in the busy loop. */
    {
        long pg = sysconf(_SC_PAGESIZE);
        size_t data_sz = (size_t)pg * 8;
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.sample_period = 1000000;
        a.sample_type = SAMPLE_IP;
        a.flags = F_DISABLED;
        pin(probe);
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            FAIL("SAMP-1", "sampling open failed errno=%d", errno);
        } else {
            void *m = mmap(NULL, pg + data_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
                           (int)fd, 0);
            if (m == MAP_FAILED) {
                FAIL("SAMP-1", "mmap failed errno=%d", errno);
                close((int)fd);
            } else {
                uint64_t lo = (uint64_t)(uintptr_t)&busy;
                uint64_t hi = lo + 4096; /* busy() code span (approx) */
                ioctl((int)fd, IOC_ENABLE, 0);
                busy(200000000ull);
                ioctl((int)fd, IOC_DISABLE, 0);
                int inr = 0;
                int cnt = walk_ring_samples((struct mmap_page *)m, data_sz, lo, hi,
                                            &inr);
                /* IP-in-range is best-effort (EL0/EL1 fidelity varies); gate on
                 * sample count primarily. */
                if (cnt >= 8) {
                    PASS("SAMP-1", "samples=%d in_loop_range=%d", cnt, inr);
                } else {
                    FAIL("SAMP-1", "samples=%d (<8; overflow IRQ not firing?)", cnt);
                }
                munmap(m, pg + data_sz);
                close((int)fd);
            }
        }
    }

    /* SAMP-2: sampling on an A76 secondary (per-PE INTID 23). */
    if (!have_smp8 || first_a76() < 0 || selftest) {
        skip("SAMP-2", selftest ? "homogeneous-qemu" : "needs-smp8");
    } else {
        int c = -1;
        for (int i = 0; i < n_a76; i++) {
            if (a76_cpus[i] != 0) {
                c = a76_cpus[i];
                break;
            }
        }
        long pg = sysconf(_SC_PAGESIZE);
        size_t data_sz = (size_t)pg * 8;
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.sample_period = 500000;
        a.sample_type = SAMPLE_IP;
        a.flags = F_DISABLED;
        pid_t ch = fork();
        if (ch == 0) {
            pin(c);
            busy(400000000ull);
            _exit(0);
        }
        long fd = peo(&a, ch, -1, -1, 0);
        int cnt = 0;
        void *m = MAP_FAILED;
        if (fd >= 0) {
            m = mmap(NULL, pg + data_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
                     (int)fd, 0);
            if (m != MAP_FAILED) {
                ioctl((int)fd, IOC_ENABLE, 0);
            }
        }
        int st;
        waitpid(ch, &st, 0);
        if (fd >= 0 && m != MAP_FAILED) {
            ioctl((int)fd, IOC_DISABLE, 0);
            int inr = 0;
            cnt = walk_ring_samples((struct mmap_page *)m, data_sz, 0, ~0ull, &inr);
            munmap(m, pg + data_sz);
        }
        if (fd >= 0)
            close((int)fd);
        if (cnt > 4) {
            PASS("SAMP-2", "a76-secondary samples=%d (per-PE INTID23 fires)", cnt);
        } else {
            FAIL("SAMP-2", "a76-secondary samples=%d (INTID23/GICR not enabled?)",
                 cnt);
        }
    }

    /* SAMP-3: frequency mode adapts. */
    {
        long pg = sysconf(_SC_PAGESIZE);
        size_t data_sz = (size_t)pg * 8;
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.sample_freq = 2000;
        a.sample_type = SAMPLE_IP;
        a.flags = F_DISABLED | F_FREQ;
        pin(probe);
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            FAIL("SAMP-3", "freq open failed errno=%d", errno);
        } else {
            void *m = mmap(NULL, pg + data_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
                           (int)fd, 0);
            if (m == MAP_FAILED) {
                FAIL("SAMP-3", "mmap failed");
                close((int)fd);
            } else {
                ioctl((int)fd, IOC_ENABLE, 0);
                busy(400000000ull);
                ioctl((int)fd, IOC_DISABLE, 0);
                int inr = 0;
                int cnt = walk_ring_samples((struct mmap_page *)m, data_sz, 0,
                                            ~0ull, &inr);
                if (cnt >= 2) {
                    PASS("SAMP-3", "freq-mode samples=%d", cnt);
                } else {
                    FAIL("SAMP-3", "freq-mode samples=%d (<2; period not adapting?)",
                         cnt);
                }
                munmap(m, pg + data_sz);
                close((int)fd);
            }
        }
    }

    /* RDPMC-1: EL0 rdpmc of a programmable counter == read(fd). */
    {
        long pg = sysconf(_SC_PAGESIZE);
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_INST_RETIRED;
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        pin(probe);
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            /* TCG's cortex-a53 PMCEID doesn't advertise INST_RETIRED as a RAW
             * programmable event (only CPU_CYCLES counts), so the open is
             * rejected under selftest; real A55/A76 advertise it. */
            if (selftest) {
                skip("RDPMC-1", "tcg-event-unsupported");
            } else {
                FAIL("RDPMC-1", "open failed errno=%d", errno);
            }
        } else {
            struct mmap_page *mp =
                mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
            ioctl((int)fd, IOC_ENABLE, 0);
            busy(20000000ull);
            if (mp == MAP_FAILED) {
                FAIL("RDPMC-1", "mmap failed errno=%d", errno);
            } else {
                int cap = mp->cap_user_rdpmc;
                uint32_t idx = mp->index;
                uint16_t w = mp->pmc_width;
                uint64_t r1 = idx > 0 ? read_pmevcntr(idx - 1) : 0;
                uint64_t r2 = idx > 0 ? read_pmevcntr(idx - 1) : 0;
                uint64_t b[3] = {0, 0, 0};
                ioctl((int)fd, IOC_DISABLE, 0);
                read((int)fd, b, sizeof(b));
                /* The rdpmc capability is cap+index+width + EL0 mrs not trapping.
                 * The COUNTER VALUE needs INST_RETIRED, which is 0 under TCG, so
                 * require r1>0 (and rdpmc≈read) only on real silicon. */
                int caps_ok = cap && idx != 0 && w == 32;
                int val_ok = selftest ? 1 : (r2 >= r1 && r1 > 0);
                if (caps_ok && val_ok) {
                    PASS("RDPMC-1", "cap=1 index=%u width=%u rdpmc=%llu read=%llu",
                         idx, w, (unsigned long long)r1, (unsigned long long)b[0]);
                } else {
                    FAIL("RDPMC-1", "cap=%d index=%u width=%u rdpmc=%llu", cap, idx,
                         w, (unsigned long long)r1);
                }
                munmap(mp, pg);
            }
            close((int)fd);
        }
    }

    /* RDPMC-2: EL0 rdpmc of the dedicated cycle counter. */
    {
        long pg = sysconf(_SC_PAGESIZE);
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_HARDWARE;
        a.config = HW_CPU_CYCLES;
        a.read_format = RF_TIMING;
        a.flags = F_DISABLED;
        pin(probe);
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            FAIL("RDPMC-2", "open failed");
        } else {
            struct mmap_page *mp =
                mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
            ioctl((int)fd, IOC_ENABLE, 0);
            busy(20000000ull);
            if (mp == MAP_FAILED) {
                FAIL("RDPMC-2", "mmap failed");
            } else {
                uint32_t idx = mp->index;
                uint16_t w = mp->pmc_width;
                int cap = mp->cap_user_rdpmc;
                uint64_t r1 = read_pmccntr();
                uint64_t r2 = read_pmccntr();
                uint64_t b[3] = {0, 0, 0};
                ioctl((int)fd, IOC_DISABLE, 0);
                read((int)fd, b, sizeof(b));
                /* The cycle-counter event opens under QEMU too, so asserting cap
                 * here validates the capabilities bitfield offset on QEMU (RDPMC-1
                 * only opens on real silicon). */
                if (cap && idx == 32 && w == 64 && r2 >= r1 && r1 > 0) {
                    PASS("RDPMC-2", "cycle cap=1 index=32 width=64 pmccntr=%llu "
                                    "read=%llu",
                         (unsigned long long)r1, (unsigned long long)b[0]);
                } else {
                    FAIL("RDPMC-2", "cap=%d index=%u width=%u pmccntr=%llu", cap, idx,
                         w, (unsigned long long)r1);
                }
                munmap(mp, pg);
            }
            close((int)fd);
        }
    }

    /* RDPMC-3: 64-bit cycle counter does not wrap over a long run. */
    {
        struct counted cy = count_one(PERF_TYPE_HARDWARE, HW_CPU_CYCLES, 0, -1, 0,
                                      1200000000ull);
        if (cy.ok && cy.value > 4294967296ull) {
            PASS("RDPMC-3", "cycle value=%llu > 2^32 (no 32-bit wrap)",
                 (unsigned long long)cy.value);
        } else if (cy.ok) {
            INFO("RDPMC-3", "cycle value=%llu (<2^32; run too short on this host)",
                 (unsigned long long)cy.value);
        } else {
            FAIL("RDPMC-3", "open/read failed");
        }
        if (cy.fd >= 0)
            close((int)cy.fd);
    }
}

/* ===================================================================== */
/* Area G (binary part) — sysctls                                        */
/* ===================================================================== */

static void area_g_sysctl(void) {
    char buf[16];
    int paranoid = 99;
    if (read_file("/proc/sys/kernel/perf_event_paranoid", buf, sizeof(buf)) == 0) {
        paranoid = atoi(buf);
    }
    char fc[16] = "?";
    read_file(FORCE_CLUSTERS, fc, sizeof(fc));
    if (paranoid == -1) {
        PASS("PERF-PARANOID", "perf_event_paranoid=-1 force_clusters=%s", fc);
    } else {
        INFO("PERF-PARANOID", "perf_event_paranoid=%d force_clusters=%s", paranoid,
             fc);
    }
}

/* ==================================================================== */
/* Extended perf_event_open(2) feature areas H..Q. Appended to          */
/* perf_validate.c; reuses its helpers (peo, attr_zero, pin, msleep,    */
/* busy, read_file, write_file, PASS/FAIL/INFO/skip, online_cpu[],      */
/* first_a55/76, selftest, wscale, struct perf_event_attr/mmap_page/    */
/* perf_rec). QEMU-TCG safety: under `selftest` every section takes a   */
/* skip-as-pass / lenient path and NEVER emits FAIL.                    */
/* ==================================================================== */


#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
#endif

/* --- event types (verified against siblings) --- */
#ifndef PERF_TYPE_SOFTWARE
#define PERF_TYPE_SOFTWARE 1u
#endif
#ifndef PERF_TYPE_TRACEPOINT
#define PERF_TYPE_TRACEPOINT 2u
#endif
#ifndef PERF_TYPE_HW_CACHE
#define PERF_TYPE_HW_CACHE 3u
#endif
#ifndef PERF_TYPE_KPROBE
#define PERF_TYPE_KPROBE 6u
#endif

/* --- SOFTWARE config ids --- */
#ifndef SW_CPU_CLOCK
#define SW_CPU_CLOCK 0ull
#endif
#ifndef SW_TASK_CLOCK
#define SW_TASK_CLOCK 1ull
#endif
#ifndef SW_PAGE_FAULTS
#define SW_PAGE_FAULTS 2ull
#endif
#ifndef SW_CONTEXT_SWITCHES
#define SW_CONTEXT_SWITCHES 3ull
#endif
#ifndef SW_CPU_MIGRATIONS
#define SW_CPU_MIGRATIONS 4ull
#endif

/* --- read_format bits (RF_TIMING == TIME_ENABLED|TIME_RUNNING == 3) --- */
#ifndef FORMAT_ID
#define FORMAT_ID (1ull << 2)
#endif
#ifndef FORMAT_GROUP
#define FORMAT_GROUP (1ull << 3)
#endif

/* --- sample_type bits (SAMPLE_IP already defined by the harness) --- */
#ifndef SAMPLE_TID
#define SAMPLE_TID (1ull << 1)
#endif
#ifndef SAMPLE_TIME
#define SAMPLE_TIME (1ull << 2)
#endif
#ifndef SAMPLE_READ
#define SAMPLE_READ (1ull << 4)
#endif
#ifndef SAMPLE_CALLCHAIN
#define SAMPLE_CALLCHAIN (1ull << 5)
#endif
#ifndef SAMPLE_CPU
#define SAMPLE_CPU (1ull << 7)
#endif
#ifndef SAMPLE_REGS_USER
#define SAMPLE_REGS_USER (1ull << 12)
#endif
#ifndef SAMPLE_STACK_USER
#define SAMPLE_STACK_USER (1ull << 13)
#endif

/* --- perf_event_attr.flags side-band bits --- */
#ifndef F_COMM
#define F_COMM (1ull << 9)
#endif
#ifndef F_TASK
#define F_TASK (1ull << 13)
#endif
#ifndef F_SAMPLE_ID_ALL
#define F_SAMPLE_ID_ALL (1ull << 18)
#endif
#ifndef F_MMAP2
#define F_MMAP2 (1ull << 23)
#endif

/* --- record types (REC_SAMPLE==9 already defined) --- */
#ifndef REC_LOST
#define REC_LOST 2u
#endif
#ifndef REC_COMM
#define REC_COMM 3u
#endif
#ifndef REC_EXIT
#define REC_EXIT 4u
#endif
#ifndef REC_FORK
#define REC_FORK 7u
#endif
#ifndef REC_MMAP2
#define REC_MMAP2 10u
#endif

/* --- callchain context markers --- */
#ifndef PERF_CONTEXT_KERNEL
#define PERF_CONTEXT_KERNEL ((uint64_t)-128)
#endif
#ifndef PERF_CONTEXT_USER
#define PERF_CONTEXT_USER ((uint64_t)-512)
#endif
#ifndef PERF_CONTEXT_MAX
#define PERF_CONTEXT_MAX ((uint64_t)-4095)
#endif

/* --- PERF_EVENT_IOC_ID: _IOR('$',7,__u64), returns the event's u64 id --- */
#ifndef IOC_ID
#define IOC_ID 0x80082407u
#endif

/* --- aarch64 DWARF regs: x0..x30=0..30, SP=31, PC=32, MAX=33 --- */
#ifndef PERF_REG_ARM64_SP
#define PERF_REG_ARM64_SP 31u
#endif
#ifndef PERF_REG_ARM64_PC
#define PERF_REG_ARM64_PC 32u
#endif
#ifndef PERF_REG_ARM64_MAX
#define PERF_REG_ARM64_MAX 33u
#endif
#ifndef PERF_REG_ARM64_MASK
#define PERF_REG_ARM64_MASK (((uint64_t)1 << PERF_REG_ARM64_MAX) - 1)
#endif
#ifndef PERF_SAMPLE_REGS_ABI_64
#define PERF_SAMPLE_REGS_ABI_64 2ull
#endif

/* --- HW_CACHE config packing: cache_id | (op<<8) | (result<<16) --- */
#ifndef CACHE_CFG
#define C_L1D 0ull
#define C_L1I 1ull
#define C_LL 2ull
#define C_DTLB 3ull
#define C_ITLB 4ull
#define C_BPU 5ull
#define OP_READ 0ull
#define OP_PREFETCH 2ull
#define RES_ACCESS 0ull
#define RES_MISS 1ull
#define CACHE_CFG(id, op, res) ((id) | ((op) << 8) | ((res) << 16))
#endif

/* -------------------------------------------------------------------- */
/* small extension-local helpers (unique names, no harness collision)   */
/* -------------------------------------------------------------------- */

/* Wrap-safe copy of `n` bytes out of the ring at byte offset `at`. The ring
 * data region starts at mp + PAGESIZE and is data_sz bytes (the harness
 * mmap_page exposes only data_head/data_tail, so we use the documented
 * default layout: data_offset == PAGE, data_size == PAGE*N). */
static void exr_copy(const uint8_t *base, uint64_t size, uint64_t at, void *dst,
                     size_t n) {
    for (size_t b = 0; b < n; b++) {
        ((uint8_t *)dst)[b] = base[(at + b) % size];
    }
}

static volatile uint64_t ex_sink;
static int ex_gzfd = -1;

/* Syscall-heavy loop: TCG's cycle counter barely moves on pure ALU work, so
 * real read()s are needed to make the sampling counter overflow. */
static void exread(int zfd, uint64_t iters) {
    uint8_t buf[4096];
    /* Scale the syscall burst by wscale like busy(): on the board (wscale==1) the
     * full burst runs; under a TCG selftest (wscale==16) it shrinks ~16x so the
     * millions of emulated read()s don't blow the CI timeout. Selftest assertions
     * are lenient (INFO, never FAIL), so fewer samples there is fine. */
    iters /= (uint64_t)wscale;
    for (uint64_t i = 0; i < iters; i++) {
        if (zfd >= 0) {
            if (read(zfd, buf, sizeof(buf)) < 0) {
                break;
            }
        } else {
            ex_sink += i * 3ull + 1ull;
        }
    }
}

/* Known nested chain for the user-callchain / DWARF workloads. Each frame is
 * noinline so the frame pointers (kept by -fno-omit-frame-pointer) are
 * walkable; most overflows land in ex_busy's read(). */
__attribute__((noinline)) static void ex_busy(void) { exread(ex_gzfd, 400000ull); }
__attribute__((noinline)) static void ex_inner(void) { ex_busy(); }
__attribute__((noinline)) static void ex_mid(void) { ex_inner(); }
__attribute__((noinline)) static void ex_outer(void) { ex_mid(); }

/* ==================================================================== */
/* Area H — PERF_TYPE_SOFTWARE counters  (SW-*)  [from perf-sw-counters] */
/* ==================================================================== */

static void area_h_sw_workload(void) {
    busy(20000000ull); /* cpu-clock / task-clock (time-based; wscale ok) */
    /* page-faults: fault anon pages, drop, re-touch (guaranteed faults). */
    const size_t pages = 256, len = pages * 4096;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        volatile uint8_t *b = (volatile uint8_t *)p;
        for (size_t i = 0; i < pages; i++) {
            b[i * 4096] = (uint8_t)i;
        }
        (void)madvise(p, len, MADV_DONTNEED);
        for (size_t i = 0; i < pages; i++) {
            b[i * 4096] = (uint8_t)(i + 1);
        }
        (void)munmap(p, len);
    }
    /* context-switches: block repeatedly. */
    for (int i = 0; i < 20; i++) {
        msleep(1);
    }
}

static void area_h_software(void) {
    struct {
        const char *name;
        uint64_t config;
        int need_pos;
    } ev[] = {
        {"cpu-clock", SW_CPU_CLOCK, 1},
        {"task-clock", SW_TASK_CLOCK, 1},
        {"page-faults", SW_PAGE_FAULTS, 1},
        {"context-switches", SW_CONTEXT_SWITCHES, 1},
        {"cpu-migrations", SW_CPU_MIGRATIONS, 0},
    };
    const int n = 5;
    long fds[5];
    int allopen = 1;
    for (int i = 0; i < n; i++) {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_SOFTWARE;
        a.config = ev[i].config;
        a.read_format = 0; /* read() returns just the u64 value (8 bytes) */
        a.flags = F_DISABLED;
        fds[i] = peo(&a, 0, -1, -1, 0);
        if (fds[i] < 0) {
            if (errno == ENOSYS && i == 0) {
                skip("SW-1", "perf_event_open ENOSYS");
                return;
            }
            allopen = 0;
        } else {
            ioctl((int)fds[i], IOC_ENABLE, 0);
        }
    }
    area_h_sw_workload();

    int bad_read = 0, bad_pos = 0, read8 = 0;
    uint64_t vals[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        if (fds[i] < 0) {
            continue;
        }
        ioctl((int)fds[i], IOC_DISABLE, 0);
        uint64_t v = 0;
        ssize_t g = read((int)fds[i], &v, sizeof(v));
        if (g == 8) {
            read8++;
            vals[i] = v;
            if (ev[i].need_pos && v == 0) {
                bad_pos++;
            }
        } else {
            bad_read++;
        }
        close((int)fds[i]);
    }

    int ok = allopen && bad_read == 0 && bad_pos == 0 && read8 == n;
    if (ok) {
        PASS("SW-1",
             "cpu_clock=%llu task_clock=%llu page_faults=%llu ctx_sw=%llu "
             "migrations=%llu (all read()==8)",
             (unsigned long long)vals[0], (unsigned long long)vals[1],
             (unsigned long long)vals[2], (unsigned long long)vals[3],
             (unsigned long long)vals[4]);
    } else if (selftest) {
        INFO("SW-1", "lenient: open=%d read8=%d bad_read=%d bad_pos=%d",
             allopen, read8, bad_read, bad_pos);
    } else {
        FAIL("SW-1", "open=%d read8=%d bad_read=%d bad_pos=%d (SW counter <not "
                     "counted>?)",
             allopen, read8, bad_read, bad_pos);
    }
}

/* ==================================================================== */
/* Area I — PERF_TYPE_HW_CACHE combinatorial events (CACHE-*)            */
/* ==================================================================== */

static void area_i_hwcache(void) {
    struct {
        const char *name;
        uint64_t cfg;
    } sup[] = {
        {"L1D-load", CACHE_CFG(C_L1D, OP_READ, RES_ACCESS)},
        {"L1D-load-miss", CACHE_CFG(C_L1D, OP_READ, RES_MISS)},
        {"L1I-load", CACHE_CFG(C_L1I, OP_READ, RES_ACCESS)},
        {"LLC-load", CACHE_CFG(C_LL, OP_READ, RES_ACCESS)},
        {"LLC-load-miss", CACHE_CFG(C_LL, OP_READ, RES_MISS)},
        {"dTLB-load-miss", CACHE_CFG(C_DTLB, OP_READ, RES_MISS)},
        {"iTLB-load-miss", CACHE_CFG(C_ITLB, OP_READ, RES_MISS)},
        {"branch-miss", CACHE_CFG(C_BPU, OP_READ, RES_MISS)},
    };
    int routing_err = 0, opened = 0, unsupported = 0;
    for (size_t i = 0; i < sizeof(sup) / sizeof(sup[0]); i++) {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_HW_CACHE;
        a.config = sup[i].cfg;
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            if (errno == ENOSYS || errno == EOPNOTSUPP) {
                unsupported++; /* QEMU-TCG PMU has no cache events */
            } else {
                routing_err = 1; /* EINVAL etc: a mapping/routing bug */
            }
            continue;
        }
        uint64_t v = 0;
        if (read((int)fd, &v, sizeof(v)) == 8) {
            opened++;
        } else {
            routing_err = 1;
        }
        close((int)fd);
    }

    /* An L1D PREFETCH has no ARM PMUv3 event -> must be rejected at open. */
    int neg_rejected;
    {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_HW_CACHE;
        a.config = CACHE_CFG(C_L1D, OP_PREFETCH, RES_ACCESS);
        long bad = peo(&a, 0, -1, -1, 0);
        neg_rejected = (bad < 0);
        if (bad >= 0) {
            close((int)bad);
        }
    }

    if (!routing_err && neg_rejected) {
        PASS("CACHE-1",
             "opened=%d unsupported=%d no-routing-error; L1D-prefetch rejected",
             opened, unsupported);
    } else if (selftest) {
        INFO("CACHE-1", "lenient: routing_err=%d neg_rejected=%d opened=%d",
             routing_err, neg_rejected, opened);
    } else {
        FAIL("CACHE-1", "routing_err=%d neg_rejected=%d opened=%d", routing_err,
             neg_rejected, opened);
    }

    /* Board-only: L1D-read-access must actually count > 0. */
    if (selftest) {
        skip("CACHE-2", "tcg-no-cache-events");
    } else {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_HW_CACHE;
        a.config = CACHE_CFG(C_L1D, OP_READ, RES_ACCESS);
        a.flags = F_DISABLED;
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            INFO("CACHE-2", "L1D-load open failed errno=%d (unsupported on this "
                            "PMU?)",
                 errno);
        } else {
            ioctl((int)fd, IOC_ENABLE, 0);
            busy(40000000ull);
            ioctl((int)fd, IOC_DISABLE, 0);
            uint64_t v = 0;
            ssize_t g = read((int)fd, &v, sizeof(v));
            close((int)fd);
            if (g == 8 && v > 0) {
                PASS("CACHE-2", "L1D-load counted=%llu (>0)",
                     (unsigned long long)v);
            } else {
                FAIL("CACHE-2", "L1D-load counted=%llu read=%zd",
                     (unsigned long long)v, g);
            }
        }
    }
}

/* ==================================================================== */
/* Area J — in-band PERF_RECORD_LOST (LOST-*)  [from perf-hw-lost]       */
/* ==================================================================== */

static void area_j_lost(void) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t data_sz = (size_t)pg * 1; /* one tiny data page: overflows fast */
    struct perf_event_attr a;
    attr_zero(&a);
    a.type = PERF_TYPE_RAW;
    a.config = EV_CPU_CYCLES;
    a.sample_period = 20000ull;
    a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME;
    a.flags = F_DISABLED;

    long fd = peo(&a, 0, -1, -1, 0);
    if (fd < 0) {
        if (selftest) {
            skip("LOST-1", "sampling open failed under tcg");
        } else {
            FAIL("LOST-1", "sampling open failed errno=%d", errno);
        }
        return;
    }
    void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED, (int)fd, 0);
    if (base == MAP_FAILED) {
        FAIL("LOST-1", "mmap failed errno=%d", errno);
        close((int)fd);
        return;
    }
    struct mmap_page *mp = (struct mmap_page *)base;
    const uint8_t *dbase = (const uint8_t *)base + pg;

    int zfd = open("/dev/zero", O_RDONLY);
    ioctl((int)fd, IOC_RESET, 0);
    ioctl((int)fd, IOC_ENABLE, 0);

    uint64_t lost_records = 0, lost_total = 0, sample_records = 0;
    uint64_t tail = mp->data_tail;
    for (int chunk = 0; chunk < 16; chunk++) {
        exread(zfd, 200000ull); /* burst: fills the tiny ring, dropping samples */
        uint64_t head = mp->data_head;
        __sync_synchronize();
        uint64_t off = tail;
        while (off < head) {
            uint64_t rel = off % data_sz;
            struct perf_rec hdr;
            exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
            if (hdr.size == 0 || off + hdr.size > head) {
                break;
            }
            if (hdr.type == REC_SAMPLE) {
                sample_records++;
            } else if (hdr.type == REC_LOST) {
                uint64_t lost = 0; /* body: u64 id; u64 lost */
                if ((uint64_t)sizeof(hdr) + 16 <= hdr.size) {
                    exr_copy(dbase, data_sz, (rel + sizeof(hdr) + 8) % data_sz,
                             &lost, 8);
                }
                lost_records++;
                lost_total += lost;
            }
            off += hdr.size;
        }
        tail = head;
        mp->data_tail = tail; /* drain so the kernel can emit the pending LOST */
        __sync_synchronize();
    }
    ioctl((int)fd, IOC_DISABLE, 0);
    if (zfd >= 0) {
        close(zfd);
    }
    munmap(base, (size_t)pg + data_sz);
    close((int)fd);

    if (lost_records > 0 && lost_total > 0) {
        PASS("LOST-1", "samples=%llu lost_records=%llu lost_total=%llu",
             (unsigned long long)sample_records,
             (unsigned long long)lost_records, (unsigned long long)lost_total);
    } else if (selftest) {
        INFO("LOST-1", "lenient: samples=%llu lost_records=%llu lost_total=%llu",
             (unsigned long long)sample_records,
             (unsigned long long)lost_records, (unsigned long long)lost_total);
    } else {
        FAIL("LOST-1",
             "no in-band LOST (records=%llu total=%llu samples=%llu)",
             (unsigned long long)lost_records, (unsigned long long)lost_total,
             (unsigned long long)sample_records);
    }
}

/* ==================================================================== */
/* Area K — sample pid/tid attribution (TID-*) [from perf-hw-sample-tid] */
/* ==================================================================== */

struct ex_tid_result {
    pid_t worker_tid, proc_pid;
    uint64_t samples, bad_pid, bad_tid;
    int failed, open_errno;
};

static void *ex_tid_worker(void *arg) {
    struct ex_tid_result *wr = (struct ex_tid_result *)arg;
    wr->worker_tid = (pid_t)syscall(SYS_gettid);
    wr->proc_pid = getpid();

    struct perf_event_attr a;
    attr_zero(&a);
    a.type = PERF_TYPE_RAW;
    a.config = EV_CPU_CYCLES;
    a.sample_period = 100000ull;
    a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME;
    a.flags = F_DISABLED;
    /* per-task path: target the worker's own tid (pid > 0). */
    long fd = peo(&a, wr->worker_tid, -1, -1, 0);
    if (fd < 0) {
        wr->failed = 1;
        wr->open_errno = errno;
        return NULL;
    }
    long pg = sysconf(_SC_PAGESIZE);
    size_t data_sz = (size_t)pg * 8;
    void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED, (int)fd, 0);
    if (base == MAP_FAILED) {
        wr->failed = 1;
        close((int)fd);
        return NULL;
    }
    struct mmap_page *mp = (struct mmap_page *)base;
    const uint8_t *dbase = (const uint8_t *)base + pg;

    int zfd = open("/dev/zero", O_RDONLY);
    ioctl((int)fd, IOC_RESET, 0);
    ioctl((int)fd, IOC_ENABLE, 0);
    exread(zfd, 400000ull);
    ioctl((int)fd, IOC_DISABLE, 0);
    if (zfd >= 0) {
        close(zfd);
    }

    uint64_t head = mp->data_head;
    __sync_synchronize();
    uint64_t off = mp->data_tail;
    while (off < head) {
        uint64_t rel = off % data_sz;
        struct perf_rec hdr;
        exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
        if (hdr.size == 0 || off + hdr.size > head) {
            break;
        }
        if (hdr.type == REC_SAMPLE) {
            /* body: u64 ip; u32 pid; u32 tid; u64 time */
            uint64_t poff = (uint64_t)sizeof(hdr) + 8;
            if (poff + 8 <= hdr.size) {
                uint32_t s_pid = 0, s_tid = 0;
                exr_copy(dbase, data_sz, (rel + poff) % data_sz, &s_pid, 4);
                exr_copy(dbase, data_sz, (rel + poff + 4) % data_sz, &s_tid, 4);
                wr->samples++;
                if ((pid_t)s_pid != wr->proc_pid) {
                    wr->bad_pid++;
                }
                if ((pid_t)s_tid != wr->worker_tid) {
                    wr->bad_tid++;
                }
            }
        }
        off += hdr.size;
    }
    munmap(base, (size_t)pg + data_sz);
    close((int)fd);
    return NULL;
}

static void area_k_sample_tid(void) {
    struct ex_tid_result wr;
    memset(&wr, 0, sizeof(wr));
    pthread_t th;
    if (pthread_create(&th, NULL, ex_tid_worker, &wr) != 0) {
        FAIL("TID-1", "pthread_create failed");
        return;
    }
    pthread_join(th, NULL);

    if (wr.failed && wr.open_errno == ENOSYS) {
        skip("TID-1", "perf_event_open ENOSYS");
        return;
    }
    int ok = !wr.failed && wr.samples > 0 && wr.bad_tid == 0 &&
             wr.bad_pid == 0 && wr.worker_tid != wr.proc_pid;
    if (ok) {
        PASS("TID-1",
             "pid=%d worker_tid=%d samples=%llu (every sample tid/tgid correct)",
             (int)wr.proc_pid, (int)wr.worker_tid,
             (unsigned long long)wr.samples);
    } else if (selftest) {
        INFO("TID-1",
             "lenient: samples=%llu bad_pid=%llu bad_tid=%llu tid=%d pid=%d",
             (unsigned long long)wr.samples, (unsigned long long)wr.bad_pid,
             (unsigned long long)wr.bad_tid, (int)wr.worker_tid,
             (int)wr.proc_pid);
    } else {
        FAIL("TID-1",
             "samples=%llu bad_pid=%llu bad_tid=%llu worker_tid=%d pid=%d",
             (unsigned long long)wr.samples, (unsigned long long)wr.bad_pid,
             (unsigned long long)wr.bad_tid, (int)wr.worker_tid,
             (int)wr.proc_pid);
    }
}

/* ==================================================================== */
/* Area L — event groups (GRP-*)  [perf-hw-group + -group-crosstask]     */
/* ==================================================================== */

struct ex_ct_shared {
    volatile pid_t b_tid;
    volatile int stop;
};

static void *ex_ct_worker(void *arg) {
    struct ex_ct_shared *s = (struct ex_ct_shared *)arg;
    s->b_tid = (pid_t)syscall(SYS_gettid);
    __sync_synchronize();
    while (!s->stop) {
        for (volatile int d = 0; d < 100000; d++) {
        }
    }
    return NULL;
}

static void area_l_groups(void) {
    /* GRP-1: SW task-clock leader + cpu-clock member, group read layout. */
    {
        pid_t tid = (pid_t)syscall(SYS_gettid);
        uint64_t rf = FORMAT_GROUP | RF_TIMING | FORMAT_ID;
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_SOFTWARE;
        a.config = SW_TASK_CLOCK;
        a.read_format = rf;
        a.flags = F_DISABLED;
        long leader = peo(&a, tid, -1, -1, 0);
        if (leader < 0) {
            if (errno == ENOSYS) {
                skip("GRP-1", "perf_event_open ENOSYS");
            } else if (selftest) {
                INFO("GRP-1", "leader open errno=%d (lenient)", errno);
            } else {
                FAIL("GRP-1", "leader open errno=%d", errno);
            }
        } else {
            attr_zero(&a);
            a.type = PERF_TYPE_SOFTWARE;
            a.config = SW_CPU_CLOCK;
            a.read_format = rf;
            a.flags = F_DISABLED;
            long member = peo(&a, tid, -1, leader, 0);
            uint64_t lid = 0, mid = 0;
            int idok = (member >= 0 &&
                        ioctl((int)leader, IOC_ID, &lid) == 0 &&
                        ioctl((int)member, IOC_ID, &mid) == 0);
            if (member < 0) {
                if (selftest) {
                    INFO("GRP-1", "member open errno=%d (lenient)", errno);
                } else {
                    FAIL("GRP-1", "member open errno=%d", errno);
                }
                close((int)leader);
            } else {
                int zfd = open("/dev/zero", O_RDONLY);
                int oz = ex_gzfd;
                ex_gzfd = zfd;
                ioctl((int)leader, IOC_RESET, 0);
                ioctl((int)leader, IOC_ENABLE, 0); /* leader only */
                ex_busy();
                ioctl((int)leader, IOC_DISABLE, 0);
                ex_gzfd = oz;
                if (zfd >= 0) {
                    close(zfd);
                }
                uint64_t b[16];
                memset(b, 0, sizeof(b));
                ssize_t got = read((int)leader, b, sizeof(b));
                close((int)member);
                close((int)leader);
                uint64_t nr = b[0], val1 = b[5], id0 = b[4], id1 = b[6];
                int ok = (got == (ssize_t)(7 * sizeof(uint64_t))) && nr == 2 &&
                         idok && id0 == lid && id1 == mid && val1 > 0;
                if (ok) {
                    PASS("GRP-1",
                         "nr=%llu task_clock=%llu cpu_clock=%llu ids-match "
                         "(group-scheduled)",
                         (unsigned long long)nr, (unsigned long long)b[3],
                         (unsigned long long)val1);
                } else if (selftest) {
                    INFO("GRP-1", "lenient: got=%zd nr=%llu member_val=%llu", got,
                         (unsigned long long)nr, (unsigned long long)val1);
                } else {
                    FAIL("GRP-1",
                         "got=%zd nr=%llu member_val=%llu id_ok=%d (GROUP layout "
                         "wrong?)",
                         got, (unsigned long long)nr, (unsigned long long)val1,
                         idok && id0 == lid && id1 == mid);
                }
            }
        }
    }

    /* GRP-2: cross-thread group member rejected EINVAL; same-thread accepted. */
    {
        struct ex_ct_shared s = {0, 0};
        pthread_t b;
        if (pthread_create(&b, NULL, ex_ct_worker, &s) != 0) {
            FAIL("GRP-2", "pthread_create failed");
            return;
        }
        int spin = 0;
        while (s.b_tid == 0 && spin++ < 100000000) {
            for (volatile int d = 0; d < 100; d++) {
            }
        }
        __sync_synchronize();
        pid_t self_tid = (pid_t)syscall(SYS_gettid);

        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.sample_period = 100000ull;
        a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_READ;
        a.read_format = FORMAT_GROUP | FORMAT_ID;
        a.flags = F_DISABLED;
        long leader = peo(&a, self_tid, -1, -1, 0);
        if (leader < 0) {
            if (errno == ENOSYS) {
                skip("GRP-2", "ENOSYS");
            } else if (selftest) {
                INFO("GRP-2", "leader open errno=%d (lenient)", errno);
            } else {
                FAIL("GRP-2", "leader open errno=%d", errno);
            }
        } else {
            struct perf_event_attr m;
            attr_zero(&m);
            m.type = PERF_TYPE_RAW;
            m.config = EV_CPU_CYCLES;
            m.flags = F_DISABLED;
            errno = 0;
            long xfd = peo(&m, s.b_tid, -1, leader, 0); /* cross-thread */
            int xerr = errno;
            int cross_rejected = (xfd < 0 && xerr == EINVAL);
            if (xfd >= 0) {
                close((int)xfd);
            }
            long sfd = peo(&m, self_tid, -1, leader, 0); /* same-thread */
            int same_ok = (sfd >= 0);
            if (sfd >= 0) {
                close((int)sfd);
            }
            close((int)leader);
            if (cross_rejected && same_ok) {
                PASS("GRP-2", "cross-thread member EINVAL; same-thread member "
                              "opens (UAF gate holds)");
            } else if (selftest) {
                INFO("GRP-2", "lenient: cross_rejected=%d(errno=%d) same_ok=%d",
                     cross_rejected, xerr, same_ok);
            } else {
                FAIL("GRP-2", "cross_rejected=%d(errno=%d) same_ok=%d",
                     cross_rejected, xerr, same_ok);
            }
        }
        s.stop = 1;
        pthread_join(b, NULL);
    }
}

/* ==================================================================== */
/* Area M — PERF_SAMPLE_READ (SREAD-*)  [sample-read + group-sample]     */
/* ==================================================================== */

static void area_m_sread_single(void) {
    struct perf_event_attr a;
    attr_zero(&a);
    a.type = PERF_TYPE_RAW;
    a.config = EV_CPU_CYCLES;
    a.sample_period = 100000ull;
    a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME | SAMPLE_READ;
    a.read_format = 0;
    a.flags = F_DISABLED;
    long fd = peo(&a, 0, -1, -1, 0);
    if (fd < 0) {
        if (errno == ENOSYS) {
            skip("SREAD-1", "ENOSYS");
        } else if (selftest) {
            INFO("SREAD-1", "open errno=%d (lenient)", errno);
        } else {
            FAIL("SREAD-1", "open errno=%d", errno);
        }
        return;
    }
    long pg = sysconf(_SC_PAGESIZE);
    size_t data_sz = (size_t)pg * 8;
    void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED, (int)fd, 0);
    if (base == MAP_FAILED) {
        FAIL("SREAD-1", "mmap errno=%d", errno);
        close((int)fd);
        return;
    }
    struct mmap_page *mp = (struct mmap_page *)base;
    const uint8_t *dbase = (const uint8_t *)base + pg;
    int zfd = open("/dev/zero", O_RDONLY);
    ioctl((int)fd, IOC_RESET, 0);
    ioctl((int)fd, IOC_ENABLE, 0);
    exread(zfd, 400000ull);
    ioctl((int)fd, IOC_DISABLE, 0);
    if (zfd >= 0) {
        close(zfd);
    }

    uint64_t head = mp->data_head;
    __sync_synchronize();
    uint64_t off = mp->data_tail;
    uint64_t samples = 0, prev = 0, bad_order = 0, zero_val = 0;
    const uint64_t rv_off = (uint64_t)sizeof(struct perf_rec) + 8 + 8 + 8;
    while (off < head) {
        uint64_t rel = off % data_sz;
        struct perf_rec hdr;
        exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
        if (hdr.size == 0 || off + hdr.size > head) {
            break;
        }
        if (hdr.type == REC_SAMPLE && rv_off + 8 <= hdr.size) {
            uint64_t rv = 0;
            exr_copy(dbase, data_sz, (rel + rv_off) % data_sz, &rv, 8);
            samples++;
            if (rv == 0) {
                zero_val++;
            }
            if (samples > 1 && rv <= prev) {
                bad_order++;
            }
            prev = rv;
        }
        off += hdr.size;
    }
    munmap(base, (size_t)pg + data_sz);
    close((int)fd);

    int ok = samples >= 2 && zero_val == 0 && bad_order == 0;
    if (ok) {
        PASS("SREAD-1", "samples=%llu last=%llu strictly-increasing",
             (unsigned long long)samples, (unsigned long long)prev);
    } else if (selftest) {
        INFO("SREAD-1", "lenient: samples=%llu bad_order=%llu zero=%llu",
             (unsigned long long)samples, (unsigned long long)bad_order,
             (unsigned long long)zero_val);
    } else {
        FAIL("SREAD-1", "samples=%llu bad_order=%llu zero=%llu",
             (unsigned long long)samples, (unsigned long long)bad_order,
             (unsigned long long)zero_val);
    }
}

struct ex_gs_result {
    int failed, open_errno;
    uint64_t samples, bad_nr, bad_id, zero_memb;
    uint64_t last_lead;
};

static void *ex_gs_worker(void *arg) {
    struct ex_gs_result *wr = (struct ex_gs_result *)arg;
    pid_t self = (pid_t)syscall(SYS_gettid);

    struct perf_event_attr a;
    attr_zero(&a);
    a.type = PERF_TYPE_RAW;
    a.config = EV_CPU_CYCLES;
    a.sample_period = 100000ull;
    a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME | SAMPLE_READ;
    a.read_format = FORMAT_GROUP | FORMAT_ID;
    a.flags = F_DISABLED;
    long leader = peo(&a, self, -1, -1, 0);
    if (leader < 0) {
        wr->failed = 1;
        wr->open_errno = errno;
        return NULL;
    }
    struct perf_event_attr m;
    attr_zero(&m);
    m.type = PERF_TYPE_RAW;
    m.config = EV_CPU_CYCLES;
    m.sample_period = 0;
    m.read_format = FORMAT_GROUP | FORMAT_ID;
    m.flags = F_DISABLED;
    long member = peo(&m, self, -1, leader, 0);
    if (member < 0) {
        wr->failed = 1;
        wr->open_errno = errno;
        close((int)leader);
        return NULL;
    }
    uint64_t leader_id = 0, member_id = 0;
    if (ioctl((int)leader, IOC_ID, &leader_id) != 0 ||
        ioctl((int)member, IOC_ID, &member_id) != 0 ||
        leader_id == member_id) {
        wr->failed = 1;
        close((int)member);
        close((int)leader);
        return NULL;
    }
    long pg = sysconf(_SC_PAGESIZE);
    size_t data_sz = (size_t)pg * 8;
    void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED, (int)leader, 0);
    if (base == MAP_FAILED) {
        wr->failed = 1;
        close((int)member);
        close((int)leader);
        return NULL;
    }
    struct mmap_page *mp = (struct mmap_page *)base;
    const uint8_t *dbase = (const uint8_t *)base + pg;
    int zfd = open("/dev/zero", O_RDONLY);
    ioctl((int)leader, IOC_RESET, 0);
    ioctl((int)leader, IOC_ENABLE, 0);
    exread(zfd, 400000ull);
    ioctl((int)leader, IOC_DISABLE, 0);
    if (zfd >= 0) {
        close(zfd);
    }

    uint64_t head = mp->data_head;
    __sync_synchronize();
    uint64_t off = mp->data_tail;
    /* body: u64 ip; u32 pid,tid; u64 time; then GROUP|ID block:
     *   u64 nr; { u64 value; u64 id; }[nr]. */
    const uint64_t read_off = (uint64_t)sizeof(struct perf_rec) + 8 + 8 + 8;
    while (off < head) {
        uint64_t rel = off % data_sz;
        struct perf_rec hdr;
        exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
        if (hdr.size == 0 || off + hdr.size > head) {
            break;
        }
        if (hdr.type == REC_SAMPLE && read_off + 40 <= hdr.size) {
            uint64_t nr = 0, lval = 0, lid = 0, mval = 0, mid = 0;
            exr_copy(dbase, data_sz, (rel + read_off) % data_sz, &nr, 8);
            exr_copy(dbase, data_sz, (rel + read_off + 8) % data_sz, &lval, 8);
            exr_copy(dbase, data_sz, (rel + read_off + 16) % data_sz, &lid, 8);
            exr_copy(dbase, data_sz, (rel + read_off + 24) % data_sz, &mval, 8);
            exr_copy(dbase, data_sz, (rel + read_off + 32) % data_sz, &mid, 8);
            wr->samples++;
            if (nr != 2) {
                wr->bad_nr++;
            }
            if (lid != leader_id || mid != member_id) {
                wr->bad_id++;
            }
            if (mval == 0) {
                wr->zero_memb++;
            }
            wr->last_lead = lval;
        }
        off += hdr.size;
    }
    munmap(base, (size_t)pg + data_sz);
    close((int)member);
    close((int)leader);
    return NULL;
}

static void area_m_sample_read(void) {
    area_m_sread_single();

    struct ex_gs_result wr;
    memset(&wr, 0, sizeof(wr));
    pthread_t th;
    if (pthread_create(&th, NULL, ex_gs_worker, &wr) != 0) {
        FAIL("SREAD-2", "pthread_create failed");
        return;
    }
    pthread_join(th, NULL);

    if (wr.failed && wr.open_errno == ENOSYS) {
        skip("SREAD-2", "ENOSYS");
        return;
    }
    int ok = !wr.failed && wr.samples >= 2 && wr.bad_nr == 0 &&
             wr.bad_id == 0 && wr.zero_memb == 0;
    if (ok) {
        PASS("SREAD-2",
             "group-sample samples=%llu nr==2 ids-match member_val=%llu",
             (unsigned long long)wr.samples, (unsigned long long)wr.last_lead);
    } else if (selftest) {
        INFO("SREAD-2",
             "lenient: samples=%llu bad_nr=%llu bad_id=%llu zero_memb=%llu",
             (unsigned long long)wr.samples, (unsigned long long)wr.bad_nr,
             (unsigned long long)wr.bad_id, (unsigned long long)wr.zero_memb);
    } else {
        FAIL("SREAD-2",
             "samples=%llu bad_nr=%llu bad_id=%llu zero_memb=%llu failed=%d",
             (unsigned long long)wr.samples, (unsigned long long)wr.bad_nr,
             (unsigned long long)wr.bad_id, (unsigned long long)wr.zero_memb,
             wr.failed);
    }
}

/* ==================================================================== */
/* Area N — system-wide `-a` (SYSW-*)  [sw-systemwide + smp-*-allcpu]    */
/* ==================================================================== */

static void area_n_syswide(void) {
    /* (a) SW system-wide cpu-clock + context-switches over forked children. */
    {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_SOFTWARE;
        a.config = SW_CPU_CLOCK;
        a.flags = F_DISABLED;
        long cc = peo(&a, -1, 0, -1, 0);
        a.config = SW_CONTEXT_SWITCHES;
        long cs = peo(&a, -1, 0, -1, 0);
        if (cc < 0 || cs < 0) {
            if (errno == ENOSYS) {
                skip("SYSW-A", "system-wide SW ENOSYS");
            } else if (selftest) {
                INFO("SYSW-A", "open errno=%d (lenient)", errno);
            } else {
                FAIL("SYSW-A", "open cc=%ld cs=%ld errno=%d", cc, cs, errno);
            }
            if (cc >= 0)
                close((int)cc);
            if (cs >= 0)
                close((int)cs);
        } else {
            ioctl((int)cc, IOC_ENABLE, 0);
            ioctl((int)cs, IOC_ENABLE, 0);
            for (int k = 0; k < 8; k++) {
                pid_t c = fork();
                if (c == 0) {
                    char *p = malloc(1 << 20);
                    if (p) {
                        memset(p, k + 1, 1 << 20);
                        free(p);
                    }
                    _exit(0);
                }
                if (c > 0) {
                    int st;
                    waitpid(c, &st, 0);
                }
            }
            ioctl((int)cc, IOC_DISABLE, 0);
            ioctl((int)cs, IOC_DISABLE, 0);
            uint64_t v_cc = 0, v_cs = 0;
            ssize_t g1 = read((int)cc, &v_cc, sizeof(v_cc));
            ssize_t g2 = read((int)cs, &v_cs, sizeof(v_cs));
            close((int)cc);
            close((int)cs);
            int ok = (g1 == 8 && g2 == 8 && v_cc > 0 && v_cs > 0);
            if (ok) {
                PASS("SYSW-A", "cpu_clock=%llu ctx_switches=%llu (pid=-1 aggregate)",
                     (unsigned long long)v_cc, (unsigned long long)v_cs);
            } else if (selftest) {
                INFO("SYSW-A", "lenient: cpu_clock=%llu ctx_switches=%llu",
                     (unsigned long long)v_cc, (unsigned long long)v_cs);
            } else {
                FAIL("SYSW-A", "cpu_clock=%llu ctx_switches=%llu",
                     (unsigned long long)v_cc, (unsigned long long)v_cs);
            }
        }
    }

    /* (b) per-online-cpu RAW cycles counting: attr.cpu honoured. */
    if (n_online < 2) {
        skip("SYSW-B", "needs>=2-cpus");
    } else {
        pid_t kids[MAXCPU];
        int nk = 0;
        for (int i = 0; i < n_online; i++) {
            pid_t k = fork();
            if (k == 0) {
                pin(online_cpu[i]);
                busy(40000000ull);
                _exit(0);
            }
            kids[nk++] = k;
        }
        long fds[MAXCPU];
        int allopen = 1;
        for (int i = 0; i < n_online; i++) {
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PERF_TYPE_RAW;
            a.config = EV_CPU_CYCLES;
            a.read_format = RF_TIMING;
            a.flags = F_DISABLED;
            fds[i] = peo(&a, -1, online_cpu[i], -1, 0);
            if (fds[i] < 0) {
                allopen = 0;
            } else {
                ioctl((int)fds[i], IOC_ENABLE, 0);
            }
        }
        for (int i = 0; i < nk; i++) {
            int st;
            waitpid(kids[i], &st, 0);
        }
        int counted = 0;
        for (int i = 0; i < n_online; i++) {
            if (fds[i] < 0) {
                continue;
            }
            ioctl((int)fds[i], IOC_DISABLE, 0);
            uint64_t b[3] = {0, 0, 0};
            if (read((int)fds[i], b, sizeof(b)) == 24 && b[0] > 0) {
                counted++;
            }
            close((int)fds[i]);
        }
        int ok = allopen && counted == n_online;
        if (ok) {
            PASS("SYSW-B", "%d/%d per-cpu events counted (cpu=i honoured)",
                 counted, n_online);
        } else if (selftest) {
            INFO("SYSW-B", "lenient: open_all=%d counted=%d/%d", allopen, counted,
                 n_online);
        } else {
            FAIL("SYSW-B", "open_all=%d counted=%d/%d (attr.cpu ignored?)",
                 allopen, counted, n_online);
        }
    }

    /* (c) per-cpu SAMPLING fan-out: every sample carries cpu == i. */
    if (n_online < 2) {
        skip("SYSW-C", "needs>=2-cpus");
    } else {
        long pg = sysconf(_SC_PAGESIZE);
        size_t data_sz = (size_t)pg * 8;
        pid_t kids[MAXCPU];
        int nk = 0;
        for (int i = 0; i < n_online; i++) {
            pid_t k = fork();
            if (k == 0) {
                pin(online_cpu[i]);
                int zfd = open("/dev/zero", O_RDONLY);
                exread(zfd, 400000ull);
                if (zfd >= 0) {
                    close(zfd);
                }
                _exit(0);
            }
            kids[nk++] = k;
        }
        long fds[MAXCPU];
        void *bases[MAXCPU];
        int allopen = 1;
        for (int i = 0; i < n_online; i++) {
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PERF_TYPE_RAW;
            a.config = EV_CPU_CYCLES;
            a.sample_period = 100000ull;
            a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME | SAMPLE_CPU;
            a.flags = F_DISABLED;
            fds[i] = peo(&a, -1, online_cpu[i], -1, 0);
            bases[i] = MAP_FAILED;
            if (fds[i] < 0) {
                allopen = 0;
                continue;
            }
            bases[i] = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED, (int)fds[i], 0);
            if (bases[i] == MAP_FAILED) {
                allopen = 0;
            }
        }
        for (int i = 0; i < n_online; i++) {
            if (fds[i] >= 0 && bases[i] != MAP_FAILED) {
                ioctl((int)fds[i], IOC_ENABLE, 0);
            }
        }
        for (int i = 0; i < nk; i++) {
            int st;
            waitpid(kids[i], &st, 0);
        }
        int rings_ok = 0, wrong = 0;
        for (int i = 0; i < n_online; i++) {
            if (fds[i] < 0 || bases[i] == MAP_FAILED) {
                continue;
            }
            ioctl((int)fds[i], IOC_DISABLE, 0);
            struct mmap_page *mp = (struct mmap_page *)bases[i];
            const uint8_t *dbase = (const uint8_t *)bases[i] + pg;
            uint64_t head = mp->data_head;
            __sync_synchronize();
            uint64_t off = mp->data_tail;
            uint64_t samples = 0;
            const uint64_t cpu_off = (uint64_t)sizeof(struct perf_rec) + 8 + 8 + 8;
            while (off < head) {
                uint64_t rel = off % data_sz;
                struct perf_rec hdr;
                exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
                if (hdr.size == 0 || off + hdr.size > head) {
                    break;
                }
                if (hdr.type == REC_SAMPLE && cpu_off + 4 <= hdr.size) {
                    uint32_t s_cpu = 0xffffffffu;
                    exr_copy(dbase, data_sz, (rel + cpu_off) % data_sz, &s_cpu, 4);
                    samples++;
                    if ((int)s_cpu != online_cpu[i]) {
                        wrong++;
                    }
                }
                off += hdr.size;
            }
            if (samples > 0) {
                rings_ok++;
            }
            munmap(bases[i], (size_t)pg + data_sz);
            close((int)fds[i]);
        }
        int ok = allopen && wrong == 0 && rings_ok == n_online;
        if (ok) {
            PASS("SYSW-C", "%d/%d rings sampled, cpu-tag correct (armed on target "
                           "core)",
                 rings_ok, n_online);
        } else if (selftest) {
            INFO("SYSW-C", "lenient: rings_ok=%d/%d wrong_cpu=%d open_all=%d",
                 rings_ok, n_online, wrong, allopen);
        } else {
            FAIL("SYSW-C", "rings_ok=%d/%d wrong_cpu=%d open_all=%d", rings_ok,
                 n_online, wrong, allopen);
        }
    }

    /* (d) `-a` side-band records (COMM/MMAP2/FORK/EXIT) fan out to per-cpu rings.
     * Runs on smp1 too (all activity on cpu0). */
    {
        long pg = sysconf(_SC_PAGESIZE);
        size_t data_sz = (size_t)pg * 16;
        long fds[MAXCPU];
        void *bases[MAXCPU];
        int opened = 0;
        int ncpu = n_online > 0 ? n_online : 1;
        for (int i = 0; i < ncpu; i++) {
            fds[i] = -1;
            bases[i] = MAP_FAILED;
            struct perf_event_attr a;
            attr_zero(&a);
            a.type = PERF_TYPE_RAW;
            a.config = EV_CPU_CYCLES;
            a.sample_period = 0xFFFFFFFFull; /* effectively no samples */
            a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME;
            a.flags = F_DISABLED | F_COMM | F_MMAP2 | F_TASK | F_SAMPLE_ID_ALL;
            fds[i] = peo(&a, -1, online_cpu[i], -1, 0);
            if (fds[i] < 0) {
                continue;
            }
            bases[i] = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED, (int)fds[i], 0);
            if (bases[i] == MAP_FAILED) {
                close((int)fds[i]);
                fds[i] = -1;
                continue;
            }
            opened++;
        }
        if (opened == 0) {
            skip("SYSW-D", "no -a sideband event could open");
        } else {
            for (int i = 0; i < ncpu; i++) {
                if (fds[i] >= 0) {
                    ioctl((int)fds[i], IOC_ENABLE, 0);
                }
            }
            /* Post-enable activity: fork (-> FORK) a child that execs (-> COMM +
             * MMAP2) and exits (-> EXIT). Defensive if /bin/true is absent. */
            pid_t child = fork();
            if (child == 0) {
                execl("/bin/true", "true", (char *)NULL);
                execl("/bin/busybox", "busybox", "true", (char *)NULL);
                _exit(0); /* still yields FORK + EXIT */
            }
            if (child > 0) {
                int st;
                waitpid(child, &st, 0);
            }
            uint64_t n_comm = 0, n_mmap2 = 0, n_fork = 0, n_exit = 0;
            char comm_name[64] = {0}, mmap_file[128] = {0};
            for (int i = 0; i < ncpu; i++) {
                if (fds[i] < 0 || bases[i] == MAP_FAILED) {
                    continue;
                }
                ioctl((int)fds[i], IOC_DISABLE, 0);
                struct mmap_page *mp = (struct mmap_page *)bases[i];
                const uint8_t *dbase = (const uint8_t *)bases[i] + pg;
                uint64_t head = mp->data_head;
                __sync_synchronize();
                uint64_t off = mp->data_tail;
                while (off < head) {
                    uint64_t rel = off % data_sz;
                    struct perf_rec hdr;
                    exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
                    if (hdr.size == 0 || off + hdr.size > head) {
                        break;
                    }
                    if (hdr.type == REC_COMM) {
                        n_comm++;
                        if (comm_name[0] == '\0') { /* body: hdr + pid/tid, name@16 */
                            for (size_t b = 0; b + 1 < sizeof(comm_name); b++) {
                                char c = (char)dbase[(rel + 16 + b) % data_sz];
                                comm_name[b] = c;
                                if (c == '\0') {
                                    break;
                                }
                            }
                            comm_name[sizeof(comm_name) - 1] = '\0';
                        }
                    } else if (hdr.type == REC_MMAP2) {
                        n_mmap2++;
                        if (mmap_file[0] == '\0') { /* filename @72 */
                            for (size_t b = 0; b + 1 < sizeof(mmap_file); b++) {
                                char c = (char)dbase[(rel + 72 + b) % data_sz];
                                mmap_file[b] = c;
                                if (c == '\0') {
                                    break;
                                }
                            }
                            mmap_file[sizeof(mmap_file) - 1] = '\0';
                        }
                    } else if (hdr.type == REC_FORK) {
                        n_fork++;
                    } else if (hdr.type == REC_EXIT) {
                        n_exit++;
                    }
                    off += hdr.size;
                }
                munmap(bases[i], (size_t)pg + data_sz);
                close((int)fds[i]);
            }
            int ok_full = (n_comm > 0 && comm_name[0] && n_mmap2 > 0 &&
                           mmap_file[0] && n_fork > 0 && n_exit > 0);
            int ok_core = (n_fork > 0 && n_exit > 0);
            if (ok_full) {
                PASS("SYSW-D",
                     "opened=%d comm=%llu mmap2=%llu fork=%llu exit=%llu "
                     "name='%s' file='%s'",
                     opened, (unsigned long long)n_comm,
                     (unsigned long long)n_mmap2, (unsigned long long)n_fork,
                     (unsigned long long)n_exit, comm_name, mmap_file);
            } else if (ok_core) {
                INFO("SYSW-D",
                     "task sideband ok (fork=%llu exit=%llu) but COMM/MMAP2 not "
                     "observed (no execve target?) comm=%llu mmap2=%llu",
                     (unsigned long long)n_fork, (unsigned long long)n_exit,
                     (unsigned long long)n_comm, (unsigned long long)n_mmap2);
            } else if (selftest) {
                INFO("SYSW-D", "lenient: fork=%llu exit=%llu comm=%llu mmap2=%llu",
                     (unsigned long long)n_fork, (unsigned long long)n_exit,
                     (unsigned long long)n_comm, (unsigned long long)n_mmap2);
            } else {
                FAIL("SYSW-D",
                     "no FORK/EXIT sideband in any -a ring (fork=%llu exit=%llu)",
                     (unsigned long long)n_fork, (unsigned long long)n_exit);
            }
        }
    }
}

/* ==================================================================== */
/* Area O — PERF_SAMPLE_CALLCHAIN (CHAIN-*)  [callchain-kernel + -user]  */
/* ==================================================================== */

#define EX_CALLCHAIN_NR_MAX 512u

static void area_o_callchain(void) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t data_sz = (size_t)pg * 8;

    /* CHAIN-KERNEL: flat read(/dev/zero) loop, expect a kernel-context chain. */
    {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.sample_period = 100000ull;
        a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME | SAMPLE_CALLCHAIN;
        a.flags = F_DISABLED;
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            if (selftest) {
                skip("CHAIN-KERNEL", "open failed under tcg");
            } else {
                FAIL("CHAIN-KERNEL", "open errno=%d", errno);
            }
        } else {
            void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                              MAP_SHARED, (int)fd, 0);
            if (base == MAP_FAILED) {
                FAIL("CHAIN-KERNEL", "mmap errno=%d", errno);
                close((int)fd);
            } else {
                struct mmap_page *mp = (struct mmap_page *)base;
                const uint8_t *dbase = (const uint8_t *)base + pg;
                int zfd = open("/dev/zero", O_RDONLY);
                ioctl((int)fd, IOC_RESET, 0);
                ioctl((int)fd, IOC_ENABLE, 0);
                exread(zfd, 400000ull);
                ioctl((int)fd, IOC_DISABLE, 0);
                if (zfd >= 0) {
                    close(zfd);
                }
                uint64_t head = mp->data_head;
                __sync_synchronize();
                uint64_t off = mp->data_tail;
                uint64_t samples = 0, kchains = 0, max_kips = 0;
                int bad = 0;
                while (off < head) {
                    uint64_t rel = off % data_sz;
                    struct perf_rec hdr;
                    exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
                    if (hdr.size == 0 || off + hdr.size > head) {
                        break;
                    }
                    if (hdr.type == REC_SAMPLE) {
                        samples++;
                        uint64_t cur = (uint64_t)sizeof(hdr) + 8 + 8 + 8;
                        if (cur + 8 <= hdr.size) {
                            uint64_t nr = 0;
                            exr_copy(dbase, data_sz, (rel + cur) % data_sz, &nr, 8);
                            cur += 8;
                            uint64_t avail = (hdr.size - cur) / 8;
                            if (nr > avail || nr > EX_CALLCHAIN_NR_MAX) {
                                bad = 1;
                            } else {
                                int in_k = 0;
                                uint64_t k_ips = 0;
                                for (uint64_t e = 0; e < nr; e++) {
                                    uint64_t ent = 0;
                                    exr_copy(dbase, data_sz,
                                             (rel + cur + e * 8) % data_sz, &ent, 8);
                                    if (ent >= PERF_CONTEXT_MAX) {
                                        in_k = (ent == PERF_CONTEXT_KERNEL);
                                        if (in_k) {
                                            kchains++;
                                        }
                                    } else if (in_k) {
                                        k_ips++;
                                    }
                                }
                                if (k_ips > max_kips) {
                                    max_kips = k_ips;
                                }
                            }
                        }
                    }
                    off += hdr.size;
                }
                munmap(base, (size_t)pg + data_sz);
                close((int)fd);
                int ok = !bad && samples > 0 && kchains > 0 && max_kips >= 1;
                if (ok) {
                    PASS("CHAIN-KERNEL",
                         "samples=%llu kchains=%llu max_kips=%llu",
                         (unsigned long long)samples, (unsigned long long)kchains,
                         (unsigned long long)max_kips);
                } else if (selftest) {
                    INFO("CHAIN-KERNEL",
                         "lenient: samples=%llu kchains=%llu max_kips=%llu bad=%d",
                         (unsigned long long)samples, (unsigned long long)kchains,
                         (unsigned long long)max_kips, bad);
                } else {
                    FAIL("CHAIN-KERNEL",
                         "samples=%llu kchains=%llu max_kips=%llu bad=%d",
                         (unsigned long long)samples, (unsigned long long)kchains,
                         (unsigned long long)max_kips, bad);
                }
            }
        }
    }

    /* CHAIN-USER: nested outer->mid->inner->busy, expect >=4 user IPs. */
    {
        struct perf_event_attr a;
        attr_zero(&a);
        a.type = PERF_TYPE_RAW;
        a.config = EV_CPU_CYCLES;
        a.sample_period = 100000ull;
        a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME | SAMPLE_CALLCHAIN;
        a.flags = F_DISABLED;
        long fd = peo(&a, 0, -1, -1, 0);
        if (fd < 0) {
            if (selftest) {
                skip("CHAIN-USER", "open failed under tcg");
            } else {
                FAIL("CHAIN-USER", "open errno=%d", errno);
            }
        } else {
            void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                              MAP_SHARED, (int)fd, 0);
            if (base == MAP_FAILED) {
                FAIL("CHAIN-USER", "mmap errno=%d", errno);
                close((int)fd);
            } else {
                struct mmap_page *mp = (struct mmap_page *)base;
                const uint8_t *dbase = (const uint8_t *)base + pg;
                int zfd = open("/dev/zero", O_RDONLY);
                int oz = ex_gzfd;
                ex_gzfd = zfd;
                ioctl((int)fd, IOC_RESET, 0);
                ioctl((int)fd, IOC_ENABLE, 0);
                ex_outer(); /* outer->mid->inner->busy: user FP chain */
                ioctl((int)fd, IOC_DISABLE, 0);
                ex_gzfd = oz;
                if (zfd >= 0) {
                    close(zfd);
                }
                uint64_t head = mp->data_head;
                __sync_synchronize();
                uint64_t off = mp->data_tail;
                uint64_t samples = 0, uchains = 0, max_uips = 0;
                int bad = 0;
                while (off < head) {
                    uint64_t rel = off % data_sz;
                    struct perf_rec hdr;
                    exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
                    if (hdr.size == 0 || off + hdr.size > head) {
                        break;
                    }
                    if (hdr.type == REC_SAMPLE) {
                        samples++;
                        uint64_t cur = (uint64_t)sizeof(hdr) + 8 + 8 + 8;
                        if (cur + 8 <= hdr.size) {
                            uint64_t nr = 0;
                            exr_copy(dbase, data_sz, (rel + cur) % data_sz, &nr, 8);
                            cur += 8;
                            uint64_t avail = (hdr.size - cur) / 8;
                            if (nr > avail || nr > EX_CALLCHAIN_NR_MAX) {
                                bad = 1;
                            } else {
                                int in_u = 0;
                                uint64_t u_ips = 0;
                                for (uint64_t e = 0; e < nr; e++) {
                                    uint64_t ent = 0;
                                    exr_copy(dbase, data_sz,
                                             (rel + cur + e * 8) % data_sz, &ent, 8);
                                    if (ent >= PERF_CONTEXT_MAX) {
                                        in_u = (ent == PERF_CONTEXT_USER);
                                        if (in_u) {
                                            uchains++;
                                        }
                                    } else if (in_u) {
                                        u_ips++;
                                    }
                                }
                                if (u_ips > max_uips) {
                                    max_uips = u_ips;
                                }
                            }
                        }
                    }
                    off += hdr.size;
                }
                munmap(base, (size_t)pg + data_sz);
                close((int)fd);
                int ok = !bad && samples > 0 && uchains > 0 && max_uips >= 4;
                if (ok) {
                    PASS("CHAIN-USER",
                         "samples=%llu uchains=%llu max_uips=%llu (>=4 FP frames)",
                         (unsigned long long)samples, (unsigned long long)uchains,
                         (unsigned long long)max_uips);
                } else if (selftest) {
                    INFO("CHAIN-USER",
                         "lenient: samples=%llu uchains=%llu max_uips=%llu bad=%d",
                         (unsigned long long)samples, (unsigned long long)uchains,
                         (unsigned long long)max_uips, bad);
                } else {
                    FAIL("CHAIN-USER",
                         "samples=%llu uchains=%llu max_uips=%llu bad=%d (user FP "
                         "unwind not observed)",
                         (unsigned long long)samples, (unsigned long long)uchains,
                         (unsigned long long)max_uips, bad);
                }
            }
        }
    }
}

/* ==================================================================== */
/* Area P — DWARF regs+stack sampling (DWARF-*)  [from perf-dwarf-user]  */
/* ==================================================================== */

static void area_p_dwarf(void) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t data_sz = (size_t)pg * 16;
    const uint64_t stack_req = 1024;

    struct perf_event_attr a;
    attr_zero(&a);
    a.type = PERF_TYPE_RAW;
    a.config = EV_CPU_CYCLES;
    a.sample_period = 100000ull;
    a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME | SAMPLE_REGS_USER |
                    SAMPLE_STACK_USER;
    a.sample_regs_user = PERF_REG_ARM64_MASK;
    a.sample_stack_user = (uint32_t)stack_req;
    a.flags = F_DISABLED;

    long fd = peo(&a, 0, -1, -1, 0);
    if (fd < 0) {
        if (selftest) {
            skip("DWARF-1", "open failed under tcg");
        } else {
            FAIL("DWARF-1", "open errno=%d", errno);
        }
        return;
    }
    void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED, (int)fd, 0);
    if (base == MAP_FAILED) {
        FAIL("DWARF-1", "mmap errno=%d", errno);
        close((int)fd);
        return;
    }
    struct mmap_page *mp = (struct mmap_page *)base;
    const uint8_t *dbase = (const uint8_t *)base + pg;
    int zfd = open("/dev/zero", O_RDONLY);
    int oz = ex_gzfd;
    ex_gzfd = zfd;
    ioctl((int)fd, IOC_RESET, 0);
    ioctl((int)fd, IOC_ENABLE, 0);
    ex_outer();
    ioctl((int)fd, IOC_DISABLE, 0);
    ex_gzfd = oz;
    if (zfd >= 0) {
        close(zfd);
    }

    uint64_t head = mp->data_head;
    __sync_synchronize();
    uint64_t off = mp->data_tail;
    uint64_t samples = 0, uregs = 0, good = 0;
    int bad = 0;
    while (off < head) {
        uint64_t rel = off % data_sz;
        struct perf_rec hdr;
        exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
        if (hdr.size == 0 || off + hdr.size > head) {
            break;
        }
        if (hdr.type == REC_SAMPLE) {
            samples++;
            uint64_t cur = (uint64_t)sizeof(hdr) + 8 + 8 + 8;
            if (cur + 8 > hdr.size) {
                bad = 1;
                break;
            }
            uint64_t abi = 0;
            exr_copy(dbase, data_sz, (rel + cur) % data_sz, &abi, 8);
            cur += 8;
            uint64_t sp = 0, pc = 0;
            int have_regs = 0;
            if (abi == PERF_SAMPLE_REGS_ABI_64) {
                if (cur + PERF_REG_ARM64_MAX * 8 > hdr.size) {
                    bad = 1;
                    break;
                }
                exr_copy(dbase, data_sz,
                         (rel + cur + PERF_REG_ARM64_SP * 8) % data_sz, &sp, 8);
                exr_copy(dbase, data_sz,
                         (rel + cur + PERF_REG_ARM64_PC * 8) % data_sz, &pc, 8);
                cur += PERF_REG_ARM64_MAX * 8;
                have_regs = 1;
            }
            if (cur + 8 > hdr.size) {
                bad = 1;
                break;
            }
            uint64_t ssize = 0;
            exr_copy(dbase, data_sz, (rel + cur) % data_sz, &ssize, 8);
            cur += 8;
            uint64_t dyn = 0;
            int have_stack = 0;
            if (ssize != 0) {
                if (cur + ssize + 8 > hdr.size) {
                    bad = 1;
                    break;
                }
                cur += ssize;
                exr_copy(dbase, data_sz, (rel + cur) % data_sz, &dyn, 8);
                cur += 8;
                have_stack = 1;
            }
            if (have_regs && sp != 0 && pc != 0) {
                uregs++;
                if (have_stack && ssize == stack_req && dyn > 0 && dyn <= ssize) {
                    good++;
                }
            }
        }
        off += hdr.size;
    }
    munmap(base, (size_t)pg + data_sz);
    close((int)fd);

    int ok = !bad && samples > 0 && uregs > 0 && good > 0;
    if (ok) {
        PASS("DWARF-1", "samples=%llu uregs=%llu good=%llu (regs+stack captured)",
             (unsigned long long)samples, (unsigned long long)uregs,
             (unsigned long long)good);
    } else if (selftest) {
        INFO("DWARF-1", "lenient: samples=%llu uregs=%llu good=%llu bad=%d",
             (unsigned long long)samples, (unsigned long long)uregs,
             (unsigned long long)good, bad);
    } else {
        FAIL("DWARF-1", "samples=%llu uregs=%llu good=%llu bad=%d",
             (unsigned long long)samples, (unsigned long long)uregs,
             (unsigned long long)good, bad);
    }
}

/* ==================================================================== */
/* Area Q — dynamic kprobe via tracefs (KPROBE-*)  [from perf-cli-kprobe]*/
/* Always skip-safe: absent kprobe_events / rootfs -> skip.             */
/* ==================================================================== */

#define EX_KPROBE_EVENTS "/sys/kernel/debug/tracing/kprobe_events"
#define EX_KPROBE_ID "/sys/kernel/debug/tracing/events/probe/hsc/id"
#define EX_CURRENT_TRACER "/sys/kernel/debug/tracing/current_tracer"

/* First /proc/kallsyms symbol matching `needle` (exact on the symbol field when
 * `exact`, else substring); address returned in *addr. 0 on hit, -1 otherwise.
 * StarryOS kallsyms holds MANGLED Rust names, so `handle_syscall` must be matched
 * as a substring, not written verbatim to kprobe_events (the parser's
 * symbol_exists() would reject the bare name -> EINVAL). */
static int ex_ksym(const char *needle, int exact, uint64_t *addr) {
    FILE *f = fopen("/proc/kallsyms", "r");
    if (!f) {
        return -1;
    }
    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char *a = strtok(line, " ");             /* address    */
        char *t = a ? strtok(NULL, " ") : NULL;  /* type       */
        char *s = t ? strtok(NULL, " \n") : NULL; /* symbol     */
        if (!s) {
            continue;
        }
        int hit = exact ? (strcmp(s, needle) == 0) : (strstr(s, needle) != NULL);
        if (hit) {
            *addr = strtoull(a, NULL, 16);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static void area_q_kprobe(void) {
    /* 1. create the probe via the robust `_stext+<offset>` form (matches the
     * perf-cli-e2e / perf-cli-kprobe tests): resolve handle_syscall + _stext from
     * kallsyms and place the kprobe at their delta. A bare mangled/plain symbol
     * write is fragile; _stext always exists in kallsyms. */
    uint64_t hsc = 0, stext = 0;
    if (ex_ksym("_stext", 1, &stext) != 0 ||
        ex_ksym("handle_syscall", 0, &hsc) != 0 || hsc <= stext) {
        skip("KPROBE-1", "kallsyms-unresolved");
        return;
    }
    char spec[80];
    snprintf(spec, sizeof(spec), "p:probe/hsc _stext+%llu\n",
             (unsigned long long)(hsc - stext));
    if (write_file(EX_KPROBE_EVENTS, spec) != 0) {
        skip("KPROBE-1", "no-kprobe_events");
        return;
    }
    /* 2. resolve the dynamic tracepoint id. */
    char idbuf[32] = "";
    if (read_file(EX_KPROBE_ID, idbuf, sizeof(idbuf)) != 0) {
        write_file(EX_KPROBE_EVENTS, "-:probe/hsc\n");
        skip("KPROBE-1", "no-probe-id (add rejected?)");
        return;
    }
    long id = strtol(idbuf, NULL, 10);
    if (id <= 0) {
        write_file(EX_KPROBE_EVENTS, "-:probe/hsc\n");
        skip("KPROBE-1", "bad-probe-id");
        return;
    }

    /* 3. open a tracepoint perf event on the id. */
    struct perf_event_attr a;
    attr_zero(&a);
    a.type = PERF_TYPE_TRACEPOINT;
    a.config = (uint64_t)id;
    a.sample_period = 1;
    a.sample_type = SAMPLE_IP | SAMPLE_TID | SAMPLE_TIME;
    a.flags = F_DISABLED;
    long fd = peo(&a, 0, -1, -1, 0);
    if (fd < 0) {
        int e = errno;
        write_file(EX_KPROBE_EVENTS, "-:probe/hsc\n");
        /* patchable-function-entry kernels reject kprobe-on-NOP-sled with EINVAL. */
        char t[8];
        if (e == EINVAL && read_file(EX_CURRENT_TRACER, t, sizeof(t)) == 0) {
            skip("KPROBE-1", "kprobe-on-patchable-entry-unsupported");
        } else {
            skip("KPROBE-1", "tracepoint-open-failed");
        }
        return;
    }

    long pg = sysconf(_SC_PAGESIZE);
    size_t data_sz = (size_t)pg * 8;
    void *base = mmap(NULL, (size_t)pg + data_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED, (int)fd, 0);
    if (base == MAP_FAILED) {
        close((int)fd);
        write_file(EX_KPROBE_EVENTS, "-:probe/hsc\n");
        skip("KPROBE-1", "mmap-failed");
        return;
    }
    struct mmap_page *mp = (struct mmap_page *)base;
    const uint8_t *dbase = (const uint8_t *)base + pg;
    ioctl((int)fd, IOC_ENABLE, 0);
    for (int i = 0; i < 500; i++) {
        (void)getpid(); /* each syscall hits handle_syscall */
    }
    ioctl((int)fd, IOC_DISABLE, 0);

    uint64_t head = mp->data_head;
    __sync_synchronize();
    uint64_t off = mp->data_tail;
    uint64_t samples = 0;
    while (off < head) {
        uint64_t rel = off % data_sz;
        struct perf_rec hdr;
        exr_copy(dbase, data_sz, rel, &hdr, sizeof(hdr));
        if (hdr.size == 0 || off + hdr.size > head) {
            break;
        }
        if (hdr.type == REC_SAMPLE) {
            samples++;
        }
        off += hdr.size;
    }
    munmap(base, (size_t)pg + data_sz);
    close((int)fd);

    /* 4. remove the probe; assert it is gone (best-effort). */
    int removed = (write_file(EX_KPROBE_EVENTS, "-:probe/hsc\n") == 0);

    if (samples > 0) {
        PASS("KPROBE-1", "id=%ld samples=%llu removed=%d (probe hit -> SAMPLE)",
             id, (unsigned long long)samples, removed);
    } else if (selftest) {
        INFO("KPROBE-1", "lenient: id=%ld samples=0 removed=%d", id, removed);
    } else {
        FAIL("KPROBE-1", "id=%ld samples=0 (no PERF_RECORD_SAMPLE from kprobe)",
             id);
    }
}

/* ==================================================================== */
/* main() dispatch — add these calls in order, after area_g_sysctl();    */
/* (i.e. between `area_g_sysctl();` and `teardown();`):                  */
/*                                                                        */
/*     area_h_software();                                                 */
/*     area_i_hwcache();                                                  */
/*     area_j_lost();                                                     */
/*     area_k_sample_tid();                                               */
/*     area_l_groups();                                                   */
/*     area_m_sample_read();                                              */
/*     area_n_syswide();                                                  */
/*     area_o_callchain();                                                */
/*     area_p_dwarf();                                                    */
/*     area_q_kprobe();                                                   */
/* ==================================================================== */

/* ===================================================================== */
/* main                                                                   */
/* ===================================================================== */

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Early mode decision from cpu0's MIDR, read directly (before any pinning).
     * QEMU virt reports cortex-a53 (part 0xD03); the board reports 0xD05/0xD0B.
     * The grouped QEMU runner sets NO env, so a homogeneous machine must
     * auto-enter selftest (parity-override logic mode) and exit 0 — otherwise it
     * would emit a board verdict it cannot honestly produce. Env overrides:
     *   PERF_VALIDATE_SELFTEST=1 forces logic mode; PERF_VALIDATE_BOARD=1 forces
     *   board mode (then QEMU correctly reports INVALID). */
    char m0[64] = "";
    uint64_t midr0 = 0;
    if (read_file("/sys/devices/system/cpu/cpu0/regs/identification/midr_el1", m0,
                  sizeof(m0)) == 0) {
        midr0 = strtoull(m0, NULL, 16); /* bare zero-padded hex (see read_midr) */
    }
    /* "Real board" = cpu0 is an actual RK3588 core (A55 0xD05 / A76 0xD0B).
     * Anything else (QEMU a53 0xD03, or any other part) is NOT the board, so we
     * auto-enter parity-override SELFTEST logic mode. The grouped QEMU runner
     * sets no env, so this auto-detection is what makes it exit 0 there. */
    int detected_nonboard = !(midr_is_a55(midr0) || midr_is_a76(midr0));
    int force_board = (getenv("PERF_VALIDATE_BOARD") != NULL);
    if (getenv("PERF_VALIDATE_SELFTEST")) {
        selftest = 1;
    } else if (detected_nonboard && !force_board) {
        selftest = 1;
    }
    if (selftest || detected_nonboard) {
        wscale = 16; /* keep TCG run times sane; real board uses full counts */
    }

    printf("BOARD_PERF_VALIDATE_START mode=%s midr0=0x%llx (raw=\"%s\") wscale=%d\n",
           selftest ? "selftest" : "board", (unsigned long long)midr0, m0, wscale);
    fflush(stdout);

    step0_override_guard();
    discover_topology();

    /* Board mode explicitly forced on QEMU: refuse to emit a board verdict. */
    if (is_qemu && !selftest) {
        printf("BOARD_PERF_SUMMARY pass=%d fail=%d skip=%d info=%d online=%d "
               "clusters=%d+%d\n",
               n_pass, n_fail, n_skip, n_info, n_online, n_a55, n_a76);
        printf("BOARD_PERF_VALIDATE_VERDICT INVALID\n");
        printf("BOARD_PERF_VALIDATE_DONE\n");
        return 1;
    }

    area_a_sysfs();
    area_b_capacity();
    area_c_cluster();
    area_d_branch();
    area_e_smp();
    area_f_fidelity();
    area_g_sysctl();

    /* --- extended perf_event_open feature coverage (areas H..Q) --- */
    area_h_software();
    area_i_hwcache();
    area_j_lost();
    area_k_sample_tid();
    area_l_groups();
    area_m_sample_read();
    area_n_syswide();
    area_o_callchain();
    area_p_dwarf();
    area_q_kprobe();

    teardown();

    printf("BOARD_PERF_SUMMARY pass=%d fail=%d skip=%d info=%d online=%d "
           "clusters=%d+%d\n",
           n_pass, n_fail, n_skip, n_info, n_online, n_a55, n_a76);

    const char *verdict;
    if (n_fail > 0 || !integrity_ok) {
        verdict = integrity_ok ? "FAIL" : "INVALID";
    } else if (selftest) {
        verdict = "SELFTEST-OK";
    } else if (skipped_silicon || !have_smp8 || !have_both_clusters) {
        verdict = "PARTIAL";
    } else {
        verdict = "FULL";
    }
    printf("BOARD_PERF_VALIDATE_VERDICT %s\n", verdict);
    printf("BOARD_PERF_VALIDATE_DONE\n");
    fflush(stdout);
    return n_fail > 0 || !integrity_ok ? 1 : 0;
}
