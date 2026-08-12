/* thp_nohugepage.c — board oracle for the atomic THP MADV_NOHUGEPAGE split.
 *
 * Bug (fixed): splitting a 2 MiB THP anon block to 512x4 KiB unmapped the block
 * and then allocated the leaf page table on the first re-map — so under OOM the
 * area was left torn (block unmapped, area still Size2M). The first fix attempt
 * also violated break-before-make (installed a valid table descriptor over a
 * live valid 2 MiB block with no invalidate) — an aarch64 SMP TLB-conflict
 * hazard that only manifests on real multi-core silicon. Final fix: pre-reserve
 * the leaf table (infallible commit) + proper break-before-make.
 *
 * This stresses the SMP path: sibling threads on other cores tightly READ
 * already-resident pages of a private-anon THP region (so their MMUs actively
 * cache the 2 MiB translations) while the main thread madvise(MADV_NOHUGEPAGE)
 * splits them to 4 KiB. A wrong break-before-make faults/kills a sibling; a torn
 * split corrupts data. Each iteration uses a fresh mapping (THP-lite never
 * re-promotes after a split). PASS = no crash across all iterations AND the
 * data pattern survives every split.
 *
 * Build: aarch64-linux-musl-gcc -O2 -static -pthread -o thp_nohugepage thp_nohugepage.c
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif

#define RGN_BYTES (32u * 1024u * 1024u) /* 32 MiB = 16 x 2 MiB blocks */
#define PG 4096u

static unsigned char *g_region;
static size_t g_len;
static volatile int g_go;
static volatile unsigned long g_read_sum;

static unsigned char pat(size_t page_idx) { return (unsigned char)(page_idx * 7u + 3u); }

static void fill(unsigned char *r, size_t len)
{
	for (size_t i = 0; i < len; i += PG)
		r[i] = pat(i / PG);
}

static int check(unsigned char *r, size_t len)
{
	for (size_t i = 0; i < len; i += PG)
		if (r[i] != pat(i / PG))
			return 0;
	return 1;
}

/* Tight read loop over resident pages — keeps every core's MMU walking and
 * caching the 2 MiB translations while the main thread splits them. */
static void *reader(void *arg)
{
	(void)arg;
	unsigned long s = 0;
	while (g_go) {
		for (size_t i = 0; i < g_len; i += PG)
			s += g_region[i];
	}
	g_read_sum += s;
	return NULL;
}

int main(void)
{
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 2)
		ncpu = 2;
	int nrd = (int)ncpu - 1;
	if (nrd > 16)
		nrd = 16;

	const int iters = 24;
	int ok = 1;
	int split_ok = 0, split_enosys = 0;

	for (int it = 0; it < iters && ok; it++) {
		void *m = mmap(NULL, RGN_BYTES, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) {
			printf("THP_NOHUGE iter=%d mmap FAIL\n", it);
			ok = 0;
			break;
		}
		g_region = m;
		g_len = RGN_BYTES;

		fill(g_region, g_len); /* fault in -> THP-promote to 2 MiB blocks */
		/* second touch: ensure the body blocks are resident huge pages */
		volatile unsigned char t = 0;
		for (size_t i = 0; i < g_len; i += PG)
			t ^= g_region[i];
		(void)t;

		g_go = 1;
		pthread_t rd[16];
		int started = 0;
		for (int i = 0; i < nrd; i++)
			if (pthread_create(&rd[i], NULL, reader, NULL) == 0)
				started++;

		usleep(2000); /* let readers spin so MMUs cache the 2 MiB entries */
		int rc = madvise(g_region, g_len, MADV_NOHUGEPAGE);
		if (rc == 0)
			split_ok++;
		else
			split_enosys++; /* ENOSYS/EINVAL: note, don't fail the run */
		usleep(2000);

		g_go = 0;
		for (int i = 0; i < started; i++)
			pthread_join(rd[i], NULL);

		if (!check(g_region, g_len)) {
			printf("THP_NOHUGE iter=%d DATA CORRUPT after split\n", it);
			ok = 0;
		}
		munmap(m, RGN_BYTES);
	}

	/* PASS requires no crash, data intact, and at least one real split
	 * accepted (else the syscall isn't wired and we validated nothing). */
	int pass = ok && split_ok > 0;
	printf("THP_NOHUGE iters=%d readers/iter=%d splits_ok=%d splits_rejected=%d => %s\n",
	       iters, nrd, split_ok, split_enosys, pass ? "PASS" : "FAIL");
	fflush(stdout);
	return pass ? 0 : 1;
}
