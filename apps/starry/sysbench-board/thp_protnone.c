/* thp_protnone.c — on-board validation for the not-present-huge-block UAF + leak
 * fixes (commits 2b176f37c is_table, 7dfeea99a huge-frame-free, ca395531b
 * 4K-frame-free + lazy paddr-0 guard).
 *
 * The trigger for all three: a whole-block mprotect(PROT_NONE) over a THP-promoted
 * anonymous region makes the 2 MiB block PTE non-present while retaining its
 * frame ("not-present huge block"). On aarch64 that reads is_huge()==false, so
 * before the fixes the page-table walkers misread its data frame as a page table
 * (UAF if COW-shared) and/or leaked the frame on unmap.
 *
 * This exercises, on real RK3588 8-core SMP silicon:
 *   T1  huge PROT_NONE churn      -> freeram must be STABLE (frame freed, not leaked)
 *   T2  COW-shared PROT_NONE      -> fork + both peers munmap; the old UAF would
 *                                    double-free the shared 2 MiB frame -> no crash
 *   T3  4 KiB PROT_NONE churn     -> freeram stable (4K analogue leak closed)
 *   T4  PROT_NONE still faults     -> a child reading the PROT_NONE region must SIGSEGV
 *   T5  lazy mmap (no touch)+munmap-> is_unused path, no frame, no crash
 *
 * Prints THP_PROTNONE_PASS or THP_PROTNONE_FAIL:<reason>. A parent SIGSEGV/SIGBUS,
 * a syscall error, or a freeram leak all fail the run.
 *
 * Build: aarch64-linux-musl-gcc -O2 -static -pthread -o thp_protnone thp_protnone.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

#define MB (1024UL * 1024UL)
#define HUGE (2UL * MB)
#define PG 4096UL

static volatile sig_atomic_t g_segv;
static sigjmp_buf g_jmp;

static void on_segv(int sig)
{
	(void)sig;
	g_segv = 1;
	siglongjmp(g_jmp, 1);
}

/* free bytes reported by the kernel (sysinfo.freeram * mem_unit), or 0 on error. */
static unsigned long freeram_bytes(void)
{
	struct sysinfo si;
	memset(&si, 0, sizeof si);
	if (sysinfo(&si) != 0)
		return 0;
	unsigned long unit = si.mem_unit ? si.mem_unit : 1;
	return (unsigned long)si.freeram * unit;
}

static void fail(const char *why)
{
	printf("THP_PROTNONE_FAIL:%s\n", why);
	fflush(stdout);
	_exit(1);
}

/* Touch one byte per 2 MiB so a THP block faults its whole 2 MiB frame in. */
static void touch_huge(char *p, size_t len)
{
	for (size_t off = 0; off < len; off += HUGE)
		p[off] = (char)(off >> 21);
}

/* T1: mmap a multi-MiB region, fault it, whole-region mprotect(PROT_NONE),
 * munmap — many times. freeram must not bleed away (2 MiB frames must be freed
 * on unmap of the not-present huge blocks). */
static long test_huge_churn(void)
{
	const size_t REGION = 8 * MB; /* contains whole 2 MiB blocks */
	const int ITERS = 256;        /* 2 GiB of churn: a real leak drops freeram ~2 GiB */
	unsigned long before = freeram_bytes();

	for (int i = 0; i < ITERS; i++) {
		char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			fail("T1_mmap"); /* leak-exhaustion is also a failure */
		touch_huge(p, REGION);
		if (mprotect(p, REGION, PROT_NONE) != 0)
			fail("T1_mprotect");
		if (munmap(p, REGION) != 0)
			fail("T1_munmap");
	}

	unsigned long after = freeram_bytes();
	long delta = (long)before - (long)after; /* >0 == memory lost */
	printf("T1 huge churn %d x %zuMiB: freeram %lu -> %lu MiB, delta=%ld MiB\n",
	       ITERS, REGION / MB, before / MB, after / MB, delta / (long)MB);
	return delta;
}

/* T2: the UAF case. Fault a THP region, mprotect(PROT_NONE), fork so parent+child
 * COW-share the not-present huge block, then BOTH munmap. The pre-fix walker
 * misread the shared data frame as a table and double-freed it. */
static void test_cow_uaf(void)
{
	const size_t REGION = 4 * MB;
	const int ITERS = 64;

	for (int i = 0; i < ITERS; i++) {
		char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			fail("T2_mmap");
		touch_huge(p, REGION);
		if (mprotect(p, REGION, PROT_NONE) != 0)
			fail("T2_mprotect");
		pid_t pid = fork();
		if (pid < 0)
			fail("T2_fork");
		if (pid == 0) {
			/* child: die naturally on any fault (don't inherit the
			 * parent's longjmp handler), then drop its COW view of the
			 * not-present huge block. */
			signal(SIGSEGV, SIG_DFL);
			signal(SIGBUS, SIG_DFL);
			munmap(p, REGION);
			_exit(0);
		}
		int st = 0;
		if (waitpid(pid, &st, 0) < 0)
			fail("T2_wait");
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
			fail("T2_child_abnormal");
		if (munmap(p, REGION) != 0) /* parent frees the last reference */
			fail("T2_parent_munmap");
	}
	printf("T2 COW PROT_NONE fork/munmap x%d: clean (no double-free)\n", ITERS);
}

/* T3: the 4 KiB analogue. */
static long test_4k_churn(void)
{
	const size_t REGION = 512 * PG; /* 2 MiB of 4K pages */
	const int ITERS = 512;
	unsigned long before = freeram_bytes();

	for (int i = 0; i < ITERS; i++) {
		char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			fail("T3_mmap");
		for (size_t off = 0; off < REGION; off += PG)
			p[off] = 1;
		if (mprotect(p, REGION, PROT_NONE) != 0)
			fail("T3_mprotect");
		if (munmap(p, REGION) != 0)
			fail("T3_munmap");
	}
	unsigned long after = freeram_bytes();
	long delta = (long)before - (long)after;
	printf("T3 4K churn %d x %zuKiB: freeram %lu -> %lu MiB, delta=%ld MiB\n",
	       ITERS, REGION / 1024, before / MB, after / MB, delta / (long)MB);
	return delta;
}

/* T4: PROT_NONE must still deny access (we did not accidentally make it R/W). A
 * child reads the region and must die with SIGSEGV. */
static void test_protnone_faults(void)
{
	const size_t REGION = 4 * MB;
	char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		fail("T4_mmap");
	touch_huge(p, REGION);
	if (mprotect(p, REGION, PROT_NONE) != 0)
		fail("T4_mprotect");

	pid_t pid = fork();
	if (pid < 0)
		fail("T4_fork");
	if (pid == 0) {
		signal(SIGSEGV, SIG_DFL); /* must die from the fault, not longjmp */
		signal(SIGBUS, SIG_DFL);
		volatile char c = p[0]; /* must fault */
		(void)c;
		_exit(0); /* reached only if PROT_NONE did NOT fault */
	}
	int st = 0;
	if (waitpid(pid, &st, 0) < 0)
		fail("T4_wait");
	if (!(WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS)))
		fail("T4_no_fault"); /* PROT_NONE did not deny access */
	if (munmap(p, REGION) != 0)
		fail("T4_munmap");
	printf("T4 PROT_NONE access faulted correctly (child sig=%d)\n", WTERMSIG(st));
}

/* T5: lazy mmap never faulted, then munmap — exercises the is_unused / paddr-0
 * path (no frame). Must not crash or leak. */
static void test_lazy_unmap(void)
{
	const size_t REGION = 8 * MB;
	const int ITERS = 512;
	for (int i = 0; i < ITERS; i++) {
		char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			fail("T5_mmap");
		if (munmap(p, REGION) != 0) /* never touched */
			fail("T5_munmap");
	}
	printf("T5 lazy mmap+munmap x%d: clean\n", ITERS);
}

int main(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_segv;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	printf("THP_PROTNONE_BEGIN freeram=%luMiB\n", freeram_bytes() / MB);
	fflush(stdout);

	if (sigsetjmp(g_jmp, 1)) /* a fault in the PARENT is a real failure */
		fail("parent_fault");

	/* Leak threshold: a real leak drops freeram by the loop's churn (GiB); allow
	 * generous slack for allocator/cache noise. */
	const long LEAK_MiB = 384;

	long d1 = test_huge_churn();
	test_cow_uaf();
	long d3 = test_4k_churn();
	test_protnone_faults();
	test_lazy_unmap();

	if (d1 / (long)MB > LEAK_MiB)
		fail("T1_huge_leak");
	if (d3 / (long)MB > LEAK_MiB)
		fail("T3_4k_leak");

	printf("THP_PROTNONE_PASS (huge_delta=%ldMiB 4k_delta=%ldMiB thr=%ldMiB)\n",
	       d1 / (long)MB, d3 / (long)MB, LEAK_MiB);
	fflush(stdout);
	return 0;
}
