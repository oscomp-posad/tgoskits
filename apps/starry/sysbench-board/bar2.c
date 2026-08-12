/* bar2.c — minimal 2-thread pthread_barrier bunch repro for wake-trace analysis. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
static pthread_barrier_t b;
static void *w(void *a) {
    long id = (long)a;
    pthread_barrier_wait(&b);          // the wake under study
    int c0 = sched_getcpu();
    for (volatile long i = 0; i < 60000000; i++) {} /* hold the core so residency is stable */
    int c1 = sched_getcpu();
    printf("BAR2 T%ld cpu %d %d\n", id, c0, c1);
    return 0;
}
int main(void) {
    pthread_barrier_init(&b, 0, 2);
    pthread_t t0, t1;
    pthread_create(&t0, 0, w, (void *)0);
    pthread_create(&t1, 0, w, (void *)1);
    pthread_join(t0, 0);
    pthread_join(t1, 0);
    printf("BAR2_DONE\n");
    return 0;
}
