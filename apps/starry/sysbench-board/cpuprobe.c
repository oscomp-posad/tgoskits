/* cpuprobe.c — estimate effective CPU frequency from userspace.
 * A dependent chain of `add` (1-cycle latency on Cortex-A55 and A76) executed
 * N*64 times, timed with CLOCK_MONOTONIC. Loop overhead (subs+bne per 64 adds)
 * is hidden by out-of-order issue, so cycles/sec ≈ the core's actual clock.
 * Pin with `taskset -c <cpu>` to probe a specific cluster (cpu0=A55, cpu4=A76).
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    const uint64_t N = 100000000ULL; /* outer iters; *64 adds = 6.4e9 cycles */
    uint64_t a = 1, n = N;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    __asm__ volatile(
        "1:\n\t"
        ".rept 64\n\t"
        "add %0, %0, #1\n\t"
        ".endr\n\t"
        "subs %1, %1, #1\n\t"
        "bne 1b\n\t"
        : "+r"(a), "+r"(n)
        :
        : "cc");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double cycles = (double)N * 64.0;
    printf("CPUPROBE freq_est=%.0f MHz (cycles=%.0f sec=%.4f sink=%llu)\n",
           cycles / sec / 1e6, cycles, sec, (unsigned long long)a);
    return 0;
}
