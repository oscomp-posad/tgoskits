# Scheduler benchmarks: hackbench + schbench — StarryOS vs Linux

Board-measured on OrangePi-5-Plus (RK3588). Static aarch64-musl binaries (same
binary on both OSes). This captures the **baseline** (round-robin + occupancy
scheduler) for a before → after → Linux comparison. The IPC/wakeup path has since
been optimized (see "The final optimization" below — the per-message allocator
bottleneck was root-caused and fixed); the **final** column is board-pending. Raw
baseline output in `schedbench-baselines/`.

## hackbench — messaging under load (`-p` pipe, `-g` groups; Time in seconds, LOWER = better)

| test | round-robin | occ (pre-IPC) | run1: +IPC +wake_affine | run2: +IPC, **default (no wake_affine)** | Linux |
|---|---|---|---|---|---|
| process, g=2  | 7.06  | 1.40  | 1.871 | **0.516** (2.7× < occ) | 0.027 |
| process, g=5  | 18.39 | 1.72  | 1.307 | 1.330 | 0.044 |
| process, g=10 | timeout | 241.6 | 239.8 †EFAULT | 239.1 †EFAULT | 0.077 |
| thread,  g=2  | 6.59  | 2.60  | 3.004 | 2.760 | 0.030 |
| thread,  g=5  | 17.97 | 5.16  | 9.745 | 9.520 | 0.045 |
| thread,  g=10 | 69.23 | 17.35 | 41.5 †EFAULT | 40.97 †EFAULT | 0.079 |

† g=10 (400 tasks) is **not a valid measurement**: `fork()`/`Creating workers (error:
Bad address)` — the EFAULT bug (below) — so workers never all start.

**What the two board runs actually establish (correcting run 1's read):**
- **wake_affine hurts hackbench and is correctly gated OFF** — its clearest effect is
  process g=2 (1.87 with it ON → **0.516** with it OFF, now 2.7× *below* the occ
  baseline). (Run 1 mis-attributed the thread-mode gap to wake_affine.)
- **The thread-mode regression vs the occ baseline is NOT wake_affine** — run2 (wake_affine
  OFF) is essentially identical to run1 (thread g5 9.52 vs 9.75; g10 40.97 vs 41.5). The
  occ column was measured **before** the IPC allocator fix, so the delta is either the
  IPC fix or a cross-session confound (thermal/cpufreq). Process mode *improved*, thread
  mode *regressed* — the split (thread = shared address space + one aspace mutex) hints
  at aspace-lock contention, not a uniform confound. **Needs an IPC-fix A/B to isolate.**
- **`EFAULT-diag` was inconclusive** — 0 diag lines came back, but the 32-hit cap of
  serial `warn!`s was almost certainly drowned by the 400-task serial storm (lossy
  serial). So it does NOT prove the fault is outside the instrumented guards; the next
  diagnostic must survive serial loss (a `/proc` per-branch counter read into the results
  file, or a low-noise isolated repro run **first**).

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

**EFAULT root cause — FOUND + FIXED (commit `fix(starry): align THP populate range …`).**
It was **THP**, not the allocator, the scheduler, or a cross-core race (every earlier
hypothesis here was a misattribution). `populate_area` fed `area.backend().populate()`
the caller's range directly. The page-*fault* path (`handle_page_fault`) aligns that
range to the area's `page_size`, but the **demand path** (`prepare_user_memory`, i.e.
`vm_read`/`vm_write` for a *small* kernel access — a 4-byte futex word, clone's tid
pointers) aligned only to 4 KiB. For a **2 MiB THP (COW) area**, a 4 KiB range is not
2 MiB-aligned → `CowBackend::populate`'s `pages_in` → `DynPageIter::new` returns `None`
→ `AxError::InvalidInput`, which `prepare_user_memory` then mis-mapped to
`AccessDenied → BadAddress → EFAULT`. So *first-touch of a THP huge page via a small
kernel user-access* spuriously EFAULTed on a valid pointer.

This explains everything: hackbench g=10 `fork()`/`Creating workers: Bad address`,
schbench futex `Bad address`; **"board-only"** (the board config enables THP; the QEMU
*default* config did not); **"load-dependent"** (more tasks → more THP first-touches).

**How it was pinned (board-free):** kernel `error!` diags were invisible during
userspace (the tty `claim_runtime_output()` silences the console post-boot). Once that
was temporarily disabled, the diag named `prepare:populate_err → InvalidInput`. Injecting
schbench/hackbench into the QEMU rootfs (via `debugfs`) and toggling features **isolated
it to `thp`**: THP on ⇒ reproduces, THP off ⇒ clean — no board needed.

**Fix:** `populate_area` aligns the fill range to the area's `page_size` (first touch
fills the whole huge page — the THP intent; a 4 KiB area is a no-op). Kept the
independent `NoMemory → ENOMEM` errno correction.

**Validated (QEMU smp8, THP + occ scheduler):** schbench `-m2 -t8` and hackbench
`-p -g4 -P` both run cleanly, **0 "Bad address"** — where before the same build faulted
repeatedly. Board re-run of the full sched-bench suite is now unblocked.
