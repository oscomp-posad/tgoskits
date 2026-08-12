/* thp_narrow.c — isolate the THP MADV_NOHUGEPAGE fault seen on the board.
 *
 * Board data so far: single_2M split (8 iters) is clean; a 32 MiB region faults
 * on iter=1 at the FIRST 2 MiB block boundary during first-touch (fill), i.e. a
 * THP allocation/first-touch fault on a re-mmap AFTER a prior split+munmap — not
 * in the page-table split itself. This build separates the variables:
 *   - single_2M     : split, no readers  (baseline correctness)
 *   - smp_2M        : split + readers     (SMP break-before-make, on the size
 *                                          that already works single-threaded)
 *   - nosplit_32M   : mmap/fill/verify/munmap, NO madvise (pure THP realloc)
 *   - split_32M     : mmap/fill/madvise/verify/munmap (the failing case)
 * If nosplit_32M faults too => THP alloc/free/fragmentation, independent of the
 * split. If nosplit passes but split faults => the split's frame/charge handling.
 * Prints each mmap base (VA reuse?) and, on any fault, the phase+iter+VA.
 *
 * Build: aarch64-linux-musl-gcc -O2 -static -pthread -o thp_narrow thp_narrow.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif
#define PG 4096u

static volatile const char *g_phase = "init";
static volatile int g_iter = -1;
static void *g_base;
static size_t g_span;

static void segv(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)uc;
	char b[160];
	long off = (char *)si->si_addr - (char *)g_base;
	int n = snprintf(b, sizeof b,
			 "\nSEGV phase=%s iter=%d addr=%p base=%p off=%ld inrange=%d\n",
			 g_phase, g_iter, si->si_addr, g_base, off,
			 (off >= 0 && (size_t)off < g_span));
	if (write(2, b, n) < 0)
		_exit(43);
	_exit(42);
}
#define MARK(p)                          \
	do {                             \
		g_phase = (p);           \
		__asm__ volatile("" ::: "memory"); \
	} while (0)

static unsigned char pat(size_t p) { return (unsigned char)(p * 7u + 3u); }

static volatile int g_go;
static unsigned char *g_region;
static size_t g_len;

static void *reader(void *a)
{
	(void)a;
	volatile unsigned long s = 0;
	while (g_go)
		for (size_t i = 0; i < g_len; i += PG)
			s += g_region[i];
	return NULL;
}

/* One cycle. do_split gates the madvise; nrd readers spin during it. */
static int cycle(size_t bytes, int nrd, int do_split, int iter, int verbose)
{
	g_iter = iter;
	MARK("mmap");
	unsigned char *r = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED) {
		printf("  iter=%d mmap FAIL errno=%d\n", iter, errno);
		return 2;
	}
	g_base = r;
	g_span = bytes;
	g_region = r;
	g_len = bytes;
	if (verbose)
		printf("  iter=%d base=%p\n", iter, (void *)r);

	MARK("fill");
	for (size_t i = 0; i < bytes; i += PG)
		r[i] = pat(i / PG);
	MARK("touch2");
	volatile unsigned char t = 0;
	for (size_t i = 0; i < bytes; i += PG)
		t ^= r[i];
	(void)t;

	pthread_t rd[16];
	int started = 0;
	if (nrd > 0) {
		g_go = 1;
		for (int i = 0; i < nrd && i < 16; i++)
			if (pthread_create(&rd[i], NULL, reader, NULL) == 0)
				started++;
		usleep(1500);
	}

	int rc = 0, me = 0;
	if (do_split) {
		MARK("madvise");
		rc = madvise(r, bytes, MADV_NOHUGEPAGE);
		me = errno;
	}

	if (nrd > 0) {
		usleep(1500);
		g_go = 0;
		for (int i = 0; i < started; i++)
			pthread_join(rd[i], NULL);
	}

	int ret = 0;
	MARK("verify");
	for (size_t i = 0; i < bytes; i += PG)
		if (r[i] != pat(i / PG)) {
			printf("  iter=%d CORRUPT off=%zu got=%u want=%u\n", iter, i,
			       r[i], pat(i / PG));
			ret = 1;
			break;
		}
	if (do_split && rc != 0 && ret == 0) {
		printf("  iter=%d madvise rc=%d errno=%d\n", iter, rc, me);
		ret = 2;
	}
	MARK("munmap");
	munmap(r, bytes);
	return ret;
}

static void run(const char *name, size_t bytes, int nrd, int do_split, int iters,
		int verbose)
{
	MARK(name);
	int ok = 0, corrupt = 0, rej = 0;
	for (int i = 0; i < iters; i++) {
		int rc = cycle(bytes, nrd, do_split, i, verbose);
		if (rc == 0)
			ok++;
		else if (rc == 1)
			corrupt++;
		else
			rej++;
	}
	printf("PHASE %-14s bytes=%zu readers=%d split=%d iters=%d ok=%d corrupt=%d rej=%d\n",
	       name, bytes, nrd, do_split, iters, ok, corrupt, rej);
	fflush(stdout);
}

int main(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 2)
		ncpu = 2;
	int nrd = (int)ncpu - 1;

	printf("THP_NARROW begin ncpu=%ld\n", ncpu);
	fflush(stdout);
	run("single_2M", 2u * 1024 * 1024, 0, 1, 8, 0);
	run("smp_2M", 2u * 1024 * 1024, nrd, 1, 16, 0);
	run("nosplit_32M", 32u * 1024 * 1024, 0, 0, 6, 1);
	run("split_32M", 32u * 1024 * 1024, 0, 1, 6, 1);
	printf("THP_NARROW_DONE\n");
	fflush(stdout);
	return 0;
}
