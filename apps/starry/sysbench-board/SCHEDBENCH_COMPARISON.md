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

**EFAULT root cause — narrowed by a 6-agent root-cause pass (OOM excluded):**
Two things fell out:
- **A real, independent errno-masking bug (fixed, commit `fix(starry): surface ENOMEM …`):**
  `prepare_user_memory` (the `vm_read`/`vm_write` fault-in path, `mm/access.rs`) did
  `.map_err(|_| VmError::AccessDenied)`, collapsing every `populate_area` error
  (incl. `NoMemory`) into `BadAddress`/EFAULT. Now propagates `NoMemory → ENOMEM`
  (Linux parity), so a genuine OOM can never masquerade as "Bad address" again.
- **But OOM is NOT the EFAULT cause.** The workflow showed (code-backed) that the
  faulting accesses hit *resident* pages — the futex WAIT word, and clone3's args on
  the caller's own stack — so no frame is allocated and `NoMemory` is unreachable there;
  clone's real alloc failures already return ENOMEM; a 256 KB-stack OOM would *panic*,
  and 100 MB/400 tasks on GB-scale RAM isn't exhaustion. The load-dependent `BadAddress`
  on a **valid pointer** therefore comes from one of three identity/mapping guards in
  `check_region`/`prepare_user_memory`: `try_as_thread()==None`, `is_owned_by_current()`,
  or `!can_access_range()` — all concurrency/identity conditions, not resource ones.

**Decisive next step (instrumentation landed, commit `debug(starry): name which …`):**
a bounded per-branch `EFAULT-diag` log now tags exactly which guard trips (+ task + addr)
on the first 32 failures. **One board run of hackbench g=10 / schbench names the branch**,
turning a static hypothesis into a fact. (The shipped default already removes the
`wake_affine`/migration paths, so that re-run also tests whether they're implicated.)

**Then:**
1. Board run with the diag → read the branch. If an identity guard
   (`try_as_thread`/`owned_by_current`) → scheduler `current()`/`on_cpu` publish window;
   if `can_access_range` → a concurrent-fork COW area-snapshot race.
2. Clean default re-run (wake_affine OFF) at g=2/g=5 → confirm thread-mode returns
   toward the occ column.
3. **schbench's futex EFAULT reproduces at ~5 threads** (NOT load-dependent) — a
   distinct schbench-specific futex-usage bug; the diag will name its branch too.
4. Latent: the 256 KB kernel stack (`axtask/build.rs DEFAULT_TASK_STACK_SIZE`) is a real
   ~100 MB/400-task cost (eventual panic risk), worth trimming toward 16–64 KB — but it
   is a *red herring* for this EFAULT.
