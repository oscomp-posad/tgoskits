/* rusage_acct.c — accuracy oracle for the `tickacct` CPU-time accounting
 * refactor.  With tickacct, utime/stime are accumulated on timer ticks +
 * context switches instead of on every syscall boundary (Linux
 * TICK_CPU_ACCOUNTING).  The invariant that must survive: a CPU-bound process
 * still accumulates ~wall seconds of CPU time (utime+stime), with no loss and
 * no double-counting, split mostly to utime for a user loop.
 *
 * Runs two phases, each ~`secs` wall seconds on one core:
 *   user    — pure userspace compute (utime should dominate, cpu/wall ~1)
 *   syscall — a tight raw-getpid loop (still mostly user loop overhead; stime
 *             is whatever fraction of ticks happen to sample kernel mode —
 *             small when syscalls are fast, exactly like Linux tick sampling)
 *
 * The verdict checks the load-bearing property: cpu/wall in [0.80,1.20] and
 * utime>0.  A broken accounting path shows up as cpu/wall ~0 (time lost) or
 * ~2 (double-counted).  Compare the printed numbers across StarryOS tickacct=on
 * vs off vs Linux; they should agree within tick granularity.
 *
 * Build on the board (native aarch64 gcc):  gcc -O2 -static -o rusage_acct rusage_acct.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double tv2s(struct timeval tv) { return tv.tv_sec + tv.tv_usec / 1e6; }

static double now_wall(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void snap(double *u, double *s) {
	struct rusage r;
	getrusage(RUSAGE_SELF, &r);
	*u = tv2s(r.ru_utime);
	*s = tv2s(r.ru_stime);
}

int main(int argc, char **argv) {
	double secs = argc > 1 ? atof(argv[1]) : 2.0;
	volatile unsigned long acc = 0;
	double u0, s0, u1, s1, w0, w1;

	/* Phase U: pure user busy loop.  The inner batch is large so the
	 * clock_gettime in the loop condition is amortized (~1 kHz), keeping the
	 * phase overwhelmingly userspace. */
	snap(&u0, &s0);
	w0 = now_wall();
	do {
		for (int i = 0; i < 100000; i++)
			acc += (unsigned long)i * 2654435761u;
	} while (now_wall() - w0 < secs);
	w1 = now_wall();
	snap(&u1, &s1);
	double wu = w1 - w0, uu = u1 - u0, su = s1 - s0, cu = uu + su;
	printf(
	    "RUSAGE_ACCT user    wall=%.3f utime=%.3f stime=%.3f cpu=%.3f "
	    "cpu/wall=%.2f u_frac=%.2f\n",
	    wu, uu, su, cu, wu > 0 ? cu / wu : 0, cu > 0 ? uu / cu : 0);

	/* Phase S: syscall-heavy (raw getpid to bypass the Linux vDSO). */
	snap(&u0, &s0);
	w0 = now_wall();
	do {
		for (int i = 0; i < 20000; i++)
			acc += (unsigned long)syscall(SYS_getpid);
	} while (now_wall() - w0 < secs);
	w1 = now_wall();
	snap(&u1, &s1);
	double ws = w1 - w0, us = u1 - u0, ss = s1 - s0, cs = us + ss;
	printf(
	    "RUSAGE_ACCT syscall wall=%.3f utime=%.3f stime=%.3f cpu=%.3f "
	    "cpu/wall=%.2f s_frac=%.2f\n",
	    ws, us, ss, cs, ws > 0 ? cs / ws : 0, cs > 0 ? ss / cs : 0);

	int ok = (wu > 0 && cu / wu > 0.80 && cu / wu < 1.20) &&
		 (ws > 0 && cs / ws > 0.80 && cs / ws < 1.20) && uu > 0;
	printf(
	    "RUSAGE_ACCT verdict=%s (want cpu/wall in [0.80,1.20], utime>0)\n",
	    ok ? "PASS" : "FAIL");
	printf("RUSAGE_ACCT_DONE acc=%lu\n", acc);
	return 0;
}
