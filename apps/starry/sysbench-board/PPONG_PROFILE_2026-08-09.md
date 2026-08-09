# Profiling the hackbench gap → it's an oversubscription scheduling cliff (2026-08-09)

Goal (#1): find where StarryOS's ~20–60× absolute hackbench gap vs Linux actually lives, now that
the `-T` aspace-lock serialization is fixed. Tooling: `ppong.c` (a minimal pipe ping-pong that
isolates the per-message primitive from hackbench's N×M fan-out), driven on the board by the fast
`sdboot-run.py` loop.

## Finding 1 — per-message latency is NEAR-PARITY (not the gap)

Single pair, `usec/roundtrip` (2 ctxsw + 4 syscalls + 2 pipe copies):

| | Linux | StarryOS | ratio |
|---|---|---|---|
| ppong -P | 12.5 | 23.8 | 1.9× |
| ppong -T | 16.0 | 19.0 | 1.2× |

So the per-op path (syscall + pipe copy + wake + context switch) is within ~2× of Linux. The
20–60× hackbench gap is **not** here. (schbench wakeup latency is likewise near-parity, ~14 vs 6 µs.)

## Finding 2 — the gap is a SCALING CLIFF at oversubscription

Per-pair `usec/roundtrip` with **K concurrent pairs** (each pair = 2 processes; board = 8 cores):

| K pairs (procs) | Linux (agg rt/s) | StarryOS (per-pair µs) |
|---|---|---|
| 1 (2)  | 69 564 | ~24 |
| 2 (4)  | 131 311 (1.9×) | 24.7 |
| 4 (8)  | 357 747 (5.1×) | ~26 |
| 8 (16) | 656 579 (9.4×) | **~174 (7× cliff)** |

- **Linux scales ~9.4×** from K=1→K=8 (near-perfect 8-core scaling).
- **StarryOS is flat/fine up to K=4** (8 procs on 8 cores — undersubscribed) then falls off a **~7×
  cliff at K=8**, the moment the 8 cores are **2:1 oversubscribed**.

hackbench massively oversubscribes (g5 = 200 tasks, g10 = 400 tasks on 8 cores), so **this
oversubscription cliff is the hackbench gap.** It is a scheduling-under-load problem, not a
per-message-cost problem — which is why the per-op and wake-latency fixes didn't close it.

## Next (#1 step 2 → task #57)

Root-cause the oversubscription penalty. Since per-op and wake latency are near-parity, the cliff at
2:1 load points at the **scheduler's behavior when runnable > cores**: candidates ranked —
1. **run-queue lock / scheduler contention** under many concurrent switch+wake events,
2. **wake-to-busy-core preemption path** (a woken task waiting for a timeslice instead of promptly
   preempting/being picked; StarryOS tick is 100 Hz/10 ms),
3. **migration/load-balance churn** under load.

Measure which by instrumenting the switch/wake path (à la wakeprof) under a K=8 ppong load and/or an
A/B toggling the load-balancer, tick rate, and preemption behavior.

Artifacts: `ppong.c`, `uboot-pipe-short.toml`, `uboot-ppongscale-short.toml`; logs
`/tmp/ppong-starry.log`, `/tmp/ppongscale.log`.

## Fix attempt — relaxed wake_affine gate (`wake-affine-loaded`): clean 2:1 win, hackbench INCONCLUSIVE

Root cause of the cliff (confirmed via wakeprof under K=8): the wake_affine gate is
`occ(waker) <= 1`, so under oversubscription (occ ≥ 2 on every CPU) it is **always false** →
sync ping-pong wakes fall through to `select_least_loaded` (cross-core spread) → the ~1 ms SGI
tail (wakeprof at K=8: `local` 395k p50 8 µs, but `xcore_busy` 70k with **p90 2097 µs / p99 4194 µs**,
`ipi_deliver` p50 1048 µs, `sgi_to_wfi` 13140 — cores flicker idle as tasks block on `read()`).

**Fix (`wake-affine-loaded`, feature-gated, OFF):** relax the gate to a Linux `wake_affine_weight`-
style load compare — hand off to the waker whenever it is no more loaded than the wakee's previous
CPU (`occ(waker) <= occ(prev)`, keeping the cheap `occ<=1` when prev is idle so fan-outs still
spread). A ping-pong PAIR then coalesces onto one CPU.

**Results:**
- **ppong K=8 (2:1 oversubscription): CLEAN 3.4× win** — per-pair 174 µs → ~52 µs (reproducible,
  low variance). The cliff root cause is real and this addresses it.
- **hackbench -g10 (50:1 oversubscription): INCONCLUSIVE.** Run-to-run variance is enormous
  (waloaded Pg10 across 3 iters: 1.74 / 3.28 / 6.08 s — 3.5× spread for the *same kernel*), which
  swamps any effect; the 6 s outlier suggests aggressive co-location can occasionally cause bad
  pileups at extreme load. Medians ≈ accessfast. **Not a confirmed win → not promoted.**
- **idle-poll (candidate A) rejected:** only ~4% on the K=8 cliff (cores are busy, not deep-idle;
  the 50 µs poll window rarely engages).
- schbench: m1t4 (light 1→N fan-out) slightly worse (wakeup 8→14 µs); m2t8 noisy.

**Takeaways:** (1) the 2:1 cliff is genuinely the `occ<=1` gate and is fixable; (2) but hackbench's
gap lives at *extreme* oversubscription where the dominant issue is **huge scheduler variance /
instability under load** (1.7–6 s for one kernel), a different problem than the clean 2:1 cliff.
The relaxed gate is kept feature-gated (a lever for moderate-oversubscription workloads); the next
real target is the **hackbench variance/instability at 50:1**, which the ppong 2:1 microbench does
not capture.
