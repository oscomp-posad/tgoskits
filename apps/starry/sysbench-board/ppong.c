/* ppong.c — pipe ping-pong per-message latency (isolates the hackbench primitive
 * from its N×M fan-out). Each round-trip = parent write + child read + child
 * write + parent read = 2 context switches + 4 syscalls + 2 tiny pipe copies.
 *
 * Process mode (-P, fork) vs thread mode (-T, CLONE_VM) mirrors hackbench's two
 * modes, so it directly probes the per-message cost behind the StarryOS-vs-Linux
 * hackbench gap.  Build (on the board, glibc): cc -O2 -static -o ppong ppong.c -lpthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int N = 100000;
static int B = 1; /* window: messages written before draining responses */
static int p1[2], p2[2]; /* p1: driver->responder, p2: responder->driver */
static char buf[4096];

/* Windowed protocol: driver writes B then reads B; responder reads B then writes
 * B. Batching (B>1) lets the OS amortize context switches over B messages — on a
 * kernel that batches, per-message cost falls with B; on one that switches per
 * message (synchronous wake handoff), it stays flat. So B is a direct probe of
 * over-context-switching under a producer/consumer stream (= hackbench). */
static void xfer(int wfd, int rfd, int first_write) {
	int rounds = N / B;
	for (int r = 0; r < rounds; r++) {
		if (first_write) {
			for (int i = 0; i < B; i++)
				if (write(wfd, buf, 1) != 1) _exit(1);
			for (int i = 0; i < B; i++)
				if (read(rfd, buf, 1) != 1) _exit(1);
		} else {
			for (int i = 0; i < B; i++)
				if (read(rfd, buf, 1) != 1) _exit(1);
			for (int i = 0; i < B; i++)
				if (write(wfd, buf, 1) != 1) _exit(1);
		}
	}
}

static void *responder_thread(void *a) {
	(void)a;
	xfer(p2[1], p1[0], 0);
	return 0;
}

int main(int argc, char **argv) {
	int threaded = argc > 1 && strcmp(argv[1], "-T") == 0;
	if (argc > 2) N = atoi(argv[2]);
	if (argc > 3) B = atoi(argv[3]);
	if (B < 1) B = 1;
	if (pipe(p1) || pipe(p2)) return 2;

	struct timespec t0, t1;
	pthread_t th;
	pid_t pid = 0;

	if (threaded) {
		if (pthread_create(&th, 0, responder_thread, 0)) return 3;
	} else {
		pid = fork();
		if (pid < 0) return 3;
		if (pid == 0) { xfer(p2[1], p1[0], 0); _exit(0); }
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	xfer(p1[1], p2[0], 1);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	if (threaded) pthread_join(th, 0);
	else waitpid(pid, 0, 0);

	double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
	printf("PPONG %s N=%d B=%d %.3f usec/msg\n", threaded ? "-T" : "-P", N, B, ns / 1000.0 / N);
	return 0;
}
