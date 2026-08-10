/* tickacct_desched.c — board oracle for the tickacct descheduled-billing fix.
 *
 * Bug (fixed): under `tickacct`, `on_enter`'s CPU-time baseline reset was a
 * best-effort try_lock; if it lost to a cross-CPU reader holding the thread's
 * accounting lock at the resume instant, the next timer tick billed the WHOLE
 * descheduled interval (the sleep) to the thread's utime/stime — permanently
 * inflating getrusage / CLOCK_THREAD_CPUTIME_ID. Fix = a lock-free `resume_ns`
 * floor that can never be dropped.
 *
 * A sleeping thread consumes ~0 CPU. This measures the *sleeper's own*
 * CLOCK_THREAD_CPUTIME_ID across many sleep cycles while sibling "scraper"
 * threads on other cores hammer /proc/<pid>/stat (which reads every thread's
 * CPU time under its accounting lock — the exact cross-CPU contention that
 * triggered the bug). After each 0.5 s sleep a short (~30 ms) busy burst forces
 * a post-resume tick to sample this thread: on the buggy kernel that tick would
 * add up to ~0.5 s of sleep to us; on the fixed kernel it adds only the burst.
 *
 * PASS = worst single-cycle thread-CPU delta stays well under the sleep length.
 *
 * Build: aarch64-linux-musl-gcc -O2 -static -pthread -o tickacct_desched tickacct_desched.c
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile int g_running = 1;
static pid_t g_pid;

static double thread_cpu_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static double wall_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* Hammer /proc/<pid>/stat so another core holds this process's per-thread
 * accounting lock as often as possible, racing the sleeper's resume. */
static void *scraper(void *arg)
{
	(void)arg;
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/stat", (int)g_pid);
	char buf[2048];
	while (g_running) {
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			while (read(fd, buf, sizeof buf) > 0)
				;
			close(fd);
		}
	}
	return NULL;
}

static void busy(double secs)
{
	volatile unsigned long acc = 0;
	double t0 = wall_s();
	do {
		for (int i = 0; i < 100000; i++)
			acc += (unsigned long)i * 2654435761u;
	} while (wall_s() - t0 < secs);
}

int main(void)
{
	g_pid = getpid();
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 2)
		ncpu = 2;
	int nsc = (int)ncpu - 1;
	if (nsc > 16)
		nsc = 16;

	pthread_t sc[16];
	int started = 0;
	for (int i = 0; i < nsc; i++)
		if (pthread_create(&sc[i], NULL, scraper, NULL) == 0)
			started++;

	const int cycles = 8;
	const double sleep_s = 0.5;
	const double burst_s = 0.03; /* ~3 tick periods @ 100 Hz: guarantees a tick */
	double worst = 0.0;

	for (int c = 0; c < cycles; c++) {
		double before = thread_cpu_s();
		struct timespec ts = { (time_t)sleep_s,
				       (long)((sleep_s - (time_t)sleep_s) * 1e9) };
		nanosleep(&ts, NULL);
		busy(burst_s); /* force a post-resume tick to bill this thread */
		double d = thread_cpu_s() - before;
		if (d > worst)
			worst = d;
	}

	g_running = 0;
	for (int i = 0; i < started; i++)
		pthread_join(sc[i], NULL);

	/* Legit per-cycle CPU ~= burst (~0.03 s). The 0.5 s sleep must NOT be
	 * billed. Threshold 0.15 s = 5x burst but << sleep. */
	int ok = worst < 0.15;
	printf("TICKACCT_DESCHED worst_cycle_cpu=%.3fs sleep=%.2fs burst=%.2fs "
	       "scrapers=%d => %s\n",
	       worst, sleep_s, burst_s, started, ok ? "PASS" : "FAIL");
	fflush(stdout);
	return ok ? 0 : 1;
}
