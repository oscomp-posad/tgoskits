# Scheduler benchmarks: hackbench + schbench — StarryOS vs Linux

Board-measured on OrangePi-5-Plus (RK3588). Static aarch64-musl binaries (same
binary on both OSes). This captures the **baseline** (round-robin + occupancy
scheduler) for a before → after → Linux comparison. The IPC/wakeup path has since
been optimized (see "The final optimization" below — the per-message allocator
bottleneck was root-caused and fixed); the **final** column is board-pending. Raw
baseline output in `schedbench-baselines/`.

## hackbench — messaging under load (`-p` pipe, `-g` groups; Time in seconds, LOWER = better)

| test | round-robin | occ | occ + IPC-fix + wake_affine (board run 1) | Linux |
|---|---|---|---|---|
| process, g=2  | 7.06  | 1.40  | 1.871 | 0.027 |
| process, g=5  | 18.39 | 1.72  | 1.307 | 0.044 |
| process, g=10 | timeout | 241.6 | **239.8 †EFAULT** | 0.077 |
| thread,  g=2  | 6.59  | 2.60  | 3.004 | 0.030 |
| thread,  g=5  | 17.97 | 5.16  | 9.745 | 0.045 |
| thread,  g=10 | 69.23 | 17.35 | **41.5 †EFAULT** | 0.079 |

† g=10 (400 tasks) is **not a valid measurement**: the run emitted `fork() (error: Bad
address)` / `Creating workers (error: Bad address)` — a StarryOS **EFAULT-under-load**
bug (clone/futem path faults at ~400 tasks), so workers never all start.

**Board run 1 was measured with wake_affine ON, which has since been gated OFF** (see
below) — so this column is NOT the shipped default. At the clean, EFAULT-free counts
(g=2/g=5), thread-mode is *worse* than occ (g=5: 9.7 vs 5.2, ~1.9×) — the wake_affine
regression. A clean re-run of the shipped default (occ + IPC-fix, wake_affine OFF) is
board-pending; thread-mode is expected to return toward the occ column.

## schbench — BLOCKED by the same EFAULT bug

Every schbench config fails immediately with `futex-FUTEX_WAIT: Bad address` /
`FUTEX_WAKE: Bad address` (EFAULT on the futex uaddr) and can hang the board. This is
the same `Bad address` syscall bug as hackbench g=10 — **not** a scheduler/allocator
issue. schbench cannot run on StarryOS until the futex/clone EFAULT-under-load path is
fixed. (Linux reference for when it can: -m1-t4 p50/p99 = 6/9 µs, 199 RPS; -m2-t8 =
2756/24544 µs.)

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

## What board run 1 actually showed (honest, supersedes the earlier hypothesis)

The pre-board hypothesis was that the 50–260× gap is **per-message heap allocation
against the single global TLSF lock** (`PollSet::wake` allocated a `Vec` + a fresh
64-entry `Box` per wake; `block_on` did `Arc::new(AxWaker)` per pipe read/write). That
allocation is real and was removed (see the two landed fixes below), and it remains
good hygiene — **but board run 1 did not confirm it as the hackbench lever**, and it
surfaced two things the QEMU/analysis pass could not:

1. **A StarryOS EFAULT-under-load bug is the real g=10 blocker, not the allocator.**
   At g=10 (400 tasks) hackbench prints `fork() (error: Bad address)` /
   `Creating workers (error: Bad address)`, and *every* schbench config fails with
   `futex … Bad address`. The `Bad address` (EFAULT) means the clone/futex syscall
   path faults under high task count — so the "241 s → 239 s" g=10 number was never a
   real messaging measurement (workers don't all start), and the allocator fix could
   not have "collapsed" a run that doesn't actually run. **This EFAULT path is the true
   prerequisite for any hackbench/schbench parity** and is a mm/clone/futex effort, not
   a scheduler/allocator one.

2. **wake_affine regressed the workload it was meant to help** → gated OFF (see commit
   `fix(axtask): gate wake_affine opt-in`). At the clean g=2/g=5 counts, thread-mode was
   ~1.9× *worse* than occ (g=5: 9.7 vs 5.2), worsening with concurrency — hackbench is a
   fan-out (each sender feeds 40 fds), not 1:1 pairs, so co-locating every wakee on the
   waker over-consolidates. Default now reverts to the run#14-validated occ-spread.

**Landed this session (`combined-perf`, all build + clippy + QEMU-boot + adversarial-review clean):**
- `perf(axtask)`: allocation-free `PollSet::wake` (shares `wake_from_irq`'s stack drain)
  + per-task cached `AxWaker`. Sound micro-opt; board-neutral on this workload (kept).
- `fix(axtask)`: wake_affine gated to opt-in `sched-loadbalance-wake-affine` (OFF).
- `feat(axtask)`: guarded runtime migration (newidle idle-pull + push) behind opt-in
  `sched-loadbalance-pull` / `-push` — untested on-board, for the threads>cores regime.

**EFAULT root cause — FOUND + partially fixed (commit `fix(starry): surface ENOMEM …`):**
A 5-hypothesis root-cause pass found that `prepare_user_memory` (the `vm_read`/`vm_write`
fault-in path, `os/StarryOS/kernel/src/mm/access.rs:363-365`) did
`.map_err(|_| VmError::AccessDenied)` — collapsing **every** `populate_area` failure
(including a genuine `NoMemory` frame-exhaustion) into `AccessDenied` → `BadAddress` →
EFAULT. That is why an out-of-memory during a user access surfaced as the misleading
"Bad address". Fixed: added `VmError::NoMemory → AxError::NoMemory` and propagate the
real error. This is the **diagnostic key** — a board re-run with it now reports the
*true* failure (ENOMEM vs something else) instead of hiding it. The leading underlying
suspect is the oversized **256 KB kernel stack** (`axtask/build.rs` `DEFAULT_TASK_STACK_SIZE
= 0x40000`) × ~400 tasks exhausting the frame budget.

**Next, in priority order:**
1. **Board re-run with the un-mask fix** to read the *true* g=10/schbench error
   (ENOMEM confirms the memory-pressure theory). One reset.
2. If ENOMEM: cut `DEFAULT_TASK_STACK_SIZE` (256 KB → e.g. 64 KB) — board-validate for
   stack-overflow safety — and/or make the kernel-stack alloc return ENOMEM not panic.
3. Clean re-run of the shipped default (occ + IPC-fix, wake_affine OFF) at g=2/g=5 to
   confirm thread-mode returns toward the occ column.
4. Separately: **schbench's futex EFAULT reproduces at ~5 threads** (not load-dependent)
   — a distinct schbench-specific futex-usage bug to triage after the un-mask fix
   reveals its true error too.
