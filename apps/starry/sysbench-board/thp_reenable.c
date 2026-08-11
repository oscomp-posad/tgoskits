/* thp_reenable.c — ground-truth probe for mprotect(PROT_NONE)-after-touch on a
 * THP block: re-enable and partial ops. Compares StarryOS behavior to Linux.
 *
 * Each test prints PASS/FAIL with what happened. A test that SIGSEGVs where it
 * should succeed is caught (handler) and reported.
 *
 * Build: aarch64-linux-musl-gcc -O2 -static -o thp_reenable thp_reenable.c
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MB (1024UL * 1024UL)
#define HUGE (2UL * MB)

static sigjmp_buf jb;
static volatile sig_atomic_t got_sig;
static void h(int s)
{
	got_sig = s;
	siglongjmp(jb, 1);
}

/* try to read *p; returns 0 if ok, signal number if it faulted */
static int try_read(volatile char *p)
{
	got_sig = 0;
	if (sigsetjmp(jb, 1) == 0) {
		volatile char c = *p;
		(void)c;
		return 0;
	}
	return got_sig;
}
static int try_write(volatile char *p)
{
	got_sig = 0;
	if (sigsetjmp(jb, 1) == 0) {
		*p = 1;
		return 0;
	}
	return got_sig;
}

/* Returns a 2 MiB-aligned, fully-touched 2 MiB block guaranteed to sit inside a
 * larger anonymous mapping (so THP promotion + block alignment are guaranteed).
 * rawp and rawn receive the backing mapping to munmap for cleanup. */
static char *map2m_ex(char **rawp, size_t *rawn)
{
	size_t raw = 4 * HUGE; /* 8 MiB: contains a full 2 MiB-aligned block */
	char *base = mmap(NULL, raw, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		printf("  mmap failed\n");
		exit(2);
	}
	unsigned long a = ((unsigned long)base + HUGE - 1) & ~(HUGE - 1);
	char *p = (char *)a;
	for (size_t off = 0; off < HUGE; off += 4096)
		p[off] = 1; /* dense touch -> fault the whole 2 MiB block present */
	*rawp = base;
	*rawn = raw;
	return p;
}

int main(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = h;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	printf("THP_REENABLE_BEGIN\n");

	char *raw;
	size_t rawn;

	/* A: whole-block re-enable. touch -> PROT_NONE -> PROT_READ|WRITE -> access */
	{
		char *p = map2m_ex(&raw, &rawn);
		int rc_pn = mprotect(p, HUGE, PROT_NONE);
		int rc_re = mprotect(p, HUGE, PROT_READ | PROT_WRITE);
		int sig = try_write(p);
		printf("A whole re-enable: mprotect(NONE)=%d mprotect(RW)=%d access_sig=%d => %s\n",
		       rc_pn, rc_re, sig,
		       (rc_pn == 0 && rc_re == 0 && sig == 0) ? "PASS" : "FAIL");
		munmap(raw, rawn);
	}

	/* B: partial re-enable. touch -> PROT_NONE -> PROT_READ|WRITE first 1M
	 * (cuts through the 2 MiB block). */
	{
		char *p = map2m_ex(&raw, &rawn);
		int rc_pn = mprotect(p, HUGE, PROT_NONE);
		int rc_re = mprotect(p, MB, PROT_READ | PROT_WRITE); /* partial */
		int sig_lo = try_write(p);            /* re-enabled half: expect ok */
		int sig_hi = try_read(p + HUGE - 1);  /* still PROT_NONE: expect fault */
		printf("B partial re-enable(1M): mprotect(NONE)=%d mprotect(RW,1M)=%d lo_sig=%d hi_sig=%d => %s\n",
		       rc_pn, rc_re, sig_lo, sig_hi,
		       (rc_pn == 0 && rc_re == 0 && sig_lo == 0 && sig_hi != 0) ? "PASS" : "FAIL");
		munmap(raw, rawn);
	}

	/* C: partial munmap. touch -> PROT_NONE -> munmap first 1M (cuts through the
	 * block) -> munmap rest */
	{
		char *p = map2m_ex(&raw, &rawn);
		int rc_pn = mprotect(p, HUGE, PROT_NONE);
		int rc_u1 = munmap(p, MB);           /* partial unmap of the block */
		int rc_u2 = munmap(p + MB, HUGE - MB);
		printf("C partial munmap(1M): mprotect(NONE)=%d munmap(1M)=%d munmap(rest)=%d => %s\n",
		       rc_pn, rc_u1, rc_u2,
		       (rc_pn == 0 && rc_u1 == 0 && rc_u2 == 0) ? "PASS" : "FAIL");
		munmap(raw, rawn);
	}

	/* D: the T2-style block (whole-region PROT_NONE, which does NOT split the
	 * Size2M area -> leaves not-present huge blocks) then a PARTIAL munmap that
	 * cuts through a 2 MiB block. This is the split_huge_area -> prepare path. */
	{
		size_t raw = 4 * HUGE; /* 8 MiB */
		char *base = mmap(NULL, raw, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED)
			exit(2);
		for (size_t off = 0; off < raw; off += HUGE)
			base[off] = 1; /* sparse touch, one per 2 MiB (like T2) */
		int rc_pn = mprotect(base, raw, PROT_NONE); /* WHOLE region */
		/* partial munmap cutting a 2 MiB-aligned block in the middle */
		unsigned long a = ((unsigned long)base + HUGE - 1) & ~(HUGE - 1);
		char *blk = (char *)a;
		int rc_u = munmap(blk + MB / 2, MB); /* [0.5M, 1.5M) of the block */
		int rc_uf = munmap(base, raw);       /* free the rest */
		printf("D whole-PROT_NONE + partial munmap: mprotect(NONE)=%d munmap(mid)=%d munmap(rest)=%d => %s\n",
		       rc_pn, rc_u, rc_uf,
		       (rc_pn == 0 && rc_u == 0 && rc_uf == 0) ? "PASS" : "FAIL");
	}

	/* E: same block, then PARTIAL mprotect(PROT_READ|WRITE) that cuts it. */
	{
		size_t raw = 4 * HUGE;
		char *base = mmap(NULL, raw, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED)
			exit(2);
		for (size_t off = 0; off < raw; off += HUGE)
			base[off] = 1;
		int rc_pn = mprotect(base, raw, PROT_NONE);
		unsigned long a = ((unsigned long)base + HUGE - 1) & ~(HUGE - 1);
		char *blk = (char *)a;
		int rc_re = mprotect(blk + MB / 2, MB, PROT_READ | PROT_WRITE);
		int sig = try_write(blk + MB / 2); /* re-enabled: expect ok */
		printf("E whole-PROT_NONE + partial mprotect(RW): mprotect(NONE)=%d mprotect(RW,mid)=%d access_sig=%d => %s\n",
		       rc_pn, rc_re, sig,
		       (rc_pn == 0 && rc_re == 0 && sig == 0) ? "PASS" : "FAIL");
		munmap(base, raw);
	}

	printf("THP_REENABLE_DONE\n");
	return 0;
}
