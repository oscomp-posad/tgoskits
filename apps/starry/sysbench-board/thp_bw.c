/* thp_bw.c — measure the performance benefit of transparent 2 MiB huge pages.
 * Same binary under THP-on and THP-off kernels; the kernel decides whether a
 * private-anon region is backed by 2 MiB or 4 KiB pages. Three metrics:
 *
 *   1. first-touch GB/s  — write 1 byte per 4 KiB page across a fresh region.
 *      THP huge-fault takes 1 fault per 2 MiB (512x fewer) -> faster populate.
 *   2. seqwrite GB/s     — memset the populated region (raw store bandwidth;
 *      THP helps only marginally here — mostly a control).
 *   3. randpage ns/acc   — pointer-chase one 8-byte read per pseudo-random page
 *      over a region that FAR exceeds the 4 KiB L2-TLB reach (256 MiB = 65536
 *      4 KiB pages) but fits the 2 MiB-TLB (128 entries). This is the classic
 *      THP win: 4 KiB pages thrash the TLB (walk on every access), 2 MiB pages
 *      stay resident. The clearest THP discriminator.
 *
 * Usage: thp_bw [region_MiB=256] [reps=3]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    size_t MiB = (argc > 1) ? (size_t)atoll(argv[1]) : 256;
    int reps = (argc > 2) ? atoi(argv[2]) : 3;
    const size_t HUGE = 2 * 1024 * 1024;
    size_t sz = MiB * 1024 * 1024;
    size_t npg = sz / 4096;

    for (int r = 0; r < reps; r++) {
        /* over-allocate + align up to 2 MiB so THP promotion is not blocked by a
         * misaligned start. */
        char *raw = mmap(NULL, sz + HUGE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            printf("THPBW r%d MiB=%zu mmap FAIL\n", r, MiB);
            return 1;
        }
        char *p = (char *)(((uintptr_t)raw + HUGE - 1) & ~(HUGE - 1));

        /* 1. first-touch: one store per 4 KiB page (real apps fault as they use). */
        double t0 = now();
        for (size_t off = 0; off < sz; off += 4096) p[off] = 1;
        double t1 = now();
        double ft = (double)sz / (t1 - t0) / 1e9;

        /* 2. sequential store over the now-populated region. */
        double t2 = now();
        memset(p, 2, sz);
        double t3 = now();
        double seq = (double)sz / (t3 - t2) / 1e9;

        /* 3. random-page access: LCG walk hitting one 8-byte word per page. */
        volatile uint64_t sink = 0;
        size_t iters = npg * 4;
        uint64_t idx = 1;
        double t4 = now();
        for (size_t i = 0; i < iters; i++) {
            idx = (idx * 2654435761ULL + 1) % npg;
            sink += *(volatile uint64_t *)(p + idx * 4096);
        }
        double t5 = now();
        double rnd_ns = (t5 - t4) * 1e9 / (double)iters;

        printf("THPBW r%d MiB=%zu firsttouch=%.2f GB/s seqwrite=%.2f GB/s "
               "randpage=%.1f ns/acc (sink=%llu)\n",
               r, MiB, ft, seq, rnd_ns, (unsigned long long)sink);
        munmap(raw, sz + HUGE);
    }
    return 0;
}
