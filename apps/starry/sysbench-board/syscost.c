/* syscost.c — decompose per-syscall cost so we can localize StarryOS's ~7x
 * batched-pipe gap vs Linux (13.4 vs 1.87 us/msg):
 *   getpid : raw syscall entry/exit (via syscall() to bypass the Linux vDSO)
 *   pipe   : 1-byte write+read on a pre-filled pipe (never blocks) = the pipe
 *            data path (buffer mgmt + user-copy) with no wake/switch.
 * If getpid is ~7x too, the cost is trap/entry overhead; if getpid is fast but
 * pipe is ~7x, it's the pipe implementation.  Build: cc -O2 -static -o syscost syscost.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double per(struct timespec a, struct timespec b, long n) {
	return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)n;
}

int main(int argc, char **argv) {
	long N = argc > 1 ? atol(argv[1]) : 500000;
	struct timespec t0, t1;
	volatile long s = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (long i = 0; i < N; i++) s += syscall(SYS_getpid);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("SYSCOST getpid  %.1f ns/syscall\n", per(t0, t1, N));

	int p[2];
	if (pipe(p)) return 2;
	char c = 'x';
	/* keep one byte resident so write has space and read has data: write then
	 * read each iteration on a pipe that never fills or empties -> no blocking. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (long i = 0; i < N; i++) {
		if (write(p[1], &c, 1) != 1) return 3;
		if (read(p[0], &c, 1) != 1) return 3;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("SYSCOST pipe_wr  %.1f ns/(write+read)\n", per(t0, t1, N));
	return 0;
}
