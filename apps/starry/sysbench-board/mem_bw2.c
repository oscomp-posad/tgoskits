/* mem_bw2.c — CACHE-BOUND multi-thread store-bandwidth, barrier-synchronized,
 * with per-thread core-residency reporting. Purpose: on StarryOS/RK3588 make the
 * wake_affine co-location bunch DIRECTLY VISIBLE.
 *
 * vs mem_bw.c:
 *   - 256 KiB/thread buffer (stays in L2/L3) -> measures SCHEDULER PLACEMENT, not DDR.
 *   - inner loop = 8-wide UNROLLED volatile uint64_t stores (no dead-store elim).
 *   - start sync = a REAL pthread_barrier (BLOCKS on a futex) -> triggers the
 *     StarryOS wake_affine co-location (run_queue.rs:713). Spin-yield would not.
 *   - each worker prints sched_getcpu() sampled start/mid/end so the bunch shows
 *     up in the log, e.g. bunched "T0 cpu: 4 4 4  T1 cpu: 4 4 4"
 *                    vs spread  "T0 cpu: 4 4 4  T1 cpu: 5 5 5".
 *
 * argv[1]=="pin" -> one-thread-per-core sched_setaffinity; default UNPINNED.
 *
 * Build: aarch64-linux-musl-gcc -O2 -static -pthread -o mem_bw2 mem_bw2.c
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUF        (256UL * 1024)              /* per-thread, cache-resident (L2/L3) */
#define WINDOW_NS  (1200ULL * 1000 * 1000)     /* ~1.2 s timed window per thread */

static int g_ncpu;
static int g_pin;                              /* argv[1]=="pin" */
static pthread_barrier_t g_bar;                /* the block/wake trigger */

struct arg {
	int            idx;
	unsigned long  bytes;    /* out: bytes stored in the window */
	double         elapsed;  /* out: measured seconds */
	int            cpu0, cpu1, cpu2; /* out: sched_getcpu() start / mid / end */
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void *worker(void *p)
{
	struct arg *a = p;

	if (g_pin) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(a->idx % g_ncpu, &set);
		sched_setaffinity(0, sizeof set, &set);
	}

	/* volatile -> the streaming stores are NOT dead-store-eliminated (the buffer
	 * is otherwise never read) and actually reach the cache line. */
	volatile uint64_t *m = malloc(BUF);
	if (!m)
		return NULL;
	const size_t words = BUF / sizeof(uint64_t);      /* 32768 words */
	for (size_t i = 0; i < words; i++)                 /* fault + warm the cache */
		m[i] = 0;

	/* BLOCK here until every worker has arrived. The last arriver's broadcast
	 * wakes the rest -> StarryOS places each wakee on the waker's core when
	 * occ(waker) <= 1 (run_queue.rs:713). This is the whole point. */
	pthread_barrier_wait(&g_bar);

	a->cpu0 = sched_getcpu();                           /* residency @ start */

	unsigned long bytes = 0;
	uint64_t v = 1;
	uint64_t start = now_ns();
	uint64_t mid_deadline = start + WINDOW_NS / 2;
	int sampled_mid = 0;
	uint64_t t;
	while ((t = now_ns()) - start < WINDOW_NS) {
		/* 8-wide unrolled volatile stores across the resident buffer */
		for (size_t i = 0; i + 8 <= words; i += 8) {
			m[i + 0] = v;
			m[i + 1] = v;
			m[i + 2] = v;
			m[i + 3] = v;
			m[i + 4] = v;
			m[i + 5] = v;
			m[i + 6] = v;
			m[i + 7] = v;
		}
		v++;
		bytes += words * sizeof(uint64_t);
		if (!sampled_mid && t >= mid_deadline) {
			a->cpu1 = sched_getcpu();                  /* residency @ mid */
			sampled_mid = 1;
		}
	}
	a->elapsed = (double)(now_ns() - start) / 1e9;
	a->cpu2 = sched_getcpu();                           /* residency @ end */
	if (!sampled_mid)
		a->cpu1 = a->cpu2;
	a->bytes = bytes;

	free((void *)m);
	return NULL;
}

static void run(int n)
{
	pthread_t th[8];
	struct arg args[8];

	pthread_barrier_init(&g_bar, NULL, (unsigned)n);   /* n workers, no main */
	for (int i = 0; i < n; i++) {
		args[i].idx = i;
		args[i].bytes = 0;
		args[i].elapsed = 0;
		args[i].cpu0 = args[i].cpu1 = args[i].cpu2 = -1;
		pthread_create(&th[i], NULL, worker, &args[i]);
	}
	for (int i = 0; i < n; i++)
		pthread_join(th[i], NULL);
	pthread_barrier_destroy(&g_bar);

	/* aggregate = sum of each worker's own throughput over its own window */
	double gbs = 0.0;
	for (int i = 0; i < n; i++)
		if (args[i].elapsed > 0)
			gbs += (double)args[i].bytes / args[i].elapsed / 1e9;

	printf("MEM_BW2 %s threads=%d aggregate=%.1f GB/s\n",
	       g_pin ? "PIN" : "UNPIN", n, gbs);
	for (int i = 0; i < n; i++)
		printf("  T%d cpu-residency: %d %d %d  (%.1f GB/s)\n",
		       i, args[i].cpu0, args[i].cpu1, args[i].cpu2,
		       args[i].elapsed > 0
			       ? (double)args[i].bytes / args[i].elapsed / 1e9
			       : 0.0);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	g_pin = (argc > 1 && strcmp(argv[1], "pin") == 0);
	g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (g_ncpu <= 0)
		g_ncpu = 8;

	printf("MEM_BW2_BEGIN mode=%s ncpu=%d buf=%luKiB window=%llums\n",
	       g_pin ? "PIN" : "UNPIN", g_ncpu, BUF / 1024,
	       (unsigned long long)(WINDOW_NS / 1000000ull));

	int counts[4] = { 1, 2, 4, 8 };
	for (int k = 0; k < 4; k++) {
		int n = counts[k];
		if (n > g_ncpu)
			break;                 /* run 8 only if 8 cores present */
		run(n);
	}

	printf("MEM_BW2_DONE mode=%s\n", g_pin ? "PIN" : "UNPIN");
	return 0;
}
