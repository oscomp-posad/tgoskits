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
static int p1[2], p2[2]; /* p1: driver->responder, p2: responder->driver */

static void responder(void) {
	char c;
	for (int i = 0; i < N; i++) {
		if (read(p1[0], &c, 1) != 1) _exit(1);
		if (write(p2[1], &c, 1) != 1) _exit(1);
	}
}

static void *responder_thread(void *a) {
	(void)a;
	responder();
	return 0;
}

int main(int argc, char **argv) {
	int threaded = argc > 1 && strcmp(argv[1], "-T") == 0;
	if (argc > 2) N = atoi(argv[2]);
	if (pipe(p1) || pipe(p2)) return 2;

	struct timespec t0, t1;
	char c = 'x';
	pthread_t th;
	pid_t pid = 0;

	if (threaded) {
		if (pthread_create(&th, 0, responder_thread, 0)) return 3;
	} else {
		pid = fork();
		if (pid < 0) return 3;
		if (pid == 0) { responder(); _exit(0); }
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < N; i++) {
		if (write(p1[1], &c, 1) != 1) return 4;
		if (read(p2[0], &c, 1) != 1) return 4;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	if (threaded) pthread_join(th, 0);
	else waitpid(pid, 0, 0);

	double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
	printf("PPONG %s N=%d %.3f usec/roundtrip\n", threaded ? "-T" : "-P", N, ns / 1000.0 / N);
	return 0;
}
