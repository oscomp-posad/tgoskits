# Scheduler benchmarks: hackbench + schbench — StarryOS vs Linux

Board-measured on OrangePi-5-Plus (RK3588). Static aarch64-musl binaries (same
binary on both OSes). This captures the **baseline** (round-robin + occupancy
scheduler as of this session) for a future before → after → Linux comparison once
the IPC/wakeup path is optimized. Raw output in `schedbench-baselines/`.

## hackbench — messaging under load (`-p` pipe, `-g` groups; Time in seconds, LOWER = better)

| test | StarryOS round-robin | StarryOS occ (this) | StarryOS **final** (post-opt) | Linux |
|---|---|---|---|---|
| process, g=2  | 7.06  | 1.40  | _TBD_ | 0.027 |
| process, g=5  | 18.39 | 1.72  | _TBD_ | 0.044 |
| process, g=10 | timeout (>120s) | 241.6 | _TBD_ | 0.077 |
| thread,  g=2  | 6.59  | 2.60  | _TBD_ | 0.030 |
| thread,  g=5  | 17.97 | 5.16  | _TBD_ | 0.045 |
| thread,  g=10 | 69.23 | 17.35 | _TBD_ | 0.079 |

## schbench — wakeup latency + RPS (`-r 5`; latency µs LOWER, RPS HIGHER = better)

| test | round-robin | occ (this) | **final** | Linux |
|---|---|---|---|---|
| -m1 -t4  wakeup p50 / p99 (µs) | hung | hung | _TBD_ | 6 / 9 |
| -m1 -t4  avg RPS               | hung | hung | _TBD_ | 199 |
| -m2 -t8  wakeup p50 / p99 (µs) | hung | hung | _TBD_ | 2756 / 24544 |

## Reading of the baseline (honest)

- **Both StarryOS schedulers are 50–260× slower than Linux on hackbench**, and blow up
  super-linearly at high concurrency (g=10 = 400 tasks): occ hits 241 s, round-robin
  times out (>120 s). **schbench hangs entirely** on StarryOS (both schedulers).
- The occupancy scheduler is **better** than round-robin here (e.g. process g=2: 1.4 s
  vs 7.1 s) — so the occupancy-aware placement/wake work is *not* the cause. It is a
  **deep IPC / wakeup / context-switch path gap**, common to both schedulers:
  hackbench and schbench are dominated by pipe write→wake→read→block cycles, and
  StarryOS's per-wakeup cost (and its scaling under hundreds of blocked tasks) is far
  above Linux's. The super-linear blow-up at g=10 points to an O(n) or contention term
  in the wakeup/wait-queue path.
- Contrast with sysbench (CPU-bound, independent threads), where StarryOS reaches
  0.88–1.03× of Linux: there the bottleneck is compute placement (solved), not the
  wakeup path.

## The "final" optimization target (future work)

Not `wake_affine` (occ already beats round-robin here). The lever is the **wakeup /
wait-queue / pipe path**: per-wakeup cost, wait-queue data structure scaling, IPI
batching for cross-core wakes, and the context-switch fast path. Once optimized,
re-run this exact harness (`sched-bench.sh`) to fill the **final** column and chart
round-robin → occ → final → Linux.
