# Threaded-mode hackbench gap — root cause (2026-08-07)

Board (RK3588): threaded-mode (`-T`, `clone(CLONE_VM)`) hackbench is **3–10× slower** than
process-mode (`-P`, `fork`), the *opposite* of Linux (where `-T` ≈ `-P`):

| | -P g2 / g5 / g10 | -T g2 / g5 / g10 |
|---|---|---|
| StarryOS | 0.44 / 1.9 / 2.5 s | 2.4 / 6.8 / 27 s |
| Linux | 0.029 / 0.040 / 0.071 | 0.024 / 0.044 / 0.080 |

3-probe workflow (clone-VM path / futex / thread placement) + synthesis.

## Dominant root cause: the shared, exclusive, *blocking* address-space mutex on every user copy

`os/StarryOS/kernel/src/mm/access.rs` — `check_region` (:68) and `prepare_user_memory` (:358):

```rust
let aspace_arc = thr.proc_data.aspace();      // Arc<Mutex<AddrSpace>>, ONE per process
let mut aspace = aspace_arc.lock();           // EXCLUSIVE, and this is a *sleeping* mutex
if !aspace.can_access_range(...) { ... }
aspace.populate_area(page_start, len, flags)?; // page-table walk, under the lock
```

- Taken **unconditionally and exclusively** on *every* `sys_read`/`sys_write` byte-copy (via
  `vm_read`/`vm_write`) and *every* `UserPtr`/`UserConstPtr` argument validation.
- `ax_sync::Mutex` is a **blocking** mutex — on contention the task blocks on a WaitQueue and is
  woken **cross-core**, i.e. it pays the same ~1 ms wake path this whole effort has been fighting.
- The lock is held **across a page-table walk** (`populate_area` doesn't cheaply short-circuit
  already-present pages), widening the critical section.

**The thread-vs-process asymmetry, exactly:** `CLONE_VM` threads all share the *one*
`Arc<Mutex<AddrSpace>>` (the THREAD clone branch shares `proc_data`); `fork()` gives each process a
*private* aspace (`try_clone`). A hackbench pipe ping-pong is: writer copies out (1 exclusive lock)
+ reader copies in (1 exclusive lock) + arg marshalling (`check_region`), so **all N threads of the
group fully serialize on one blocking mutex**, while forked processes route the identical ops
through private, uncontended locks. × 20 msgs × loops × fd-pairs → by far the highest-frequency
shared-state touch in the hot loop, and the only *exclusive* one.

## Ranking / what the runners-up are (and aren't)
1. **aspace exclusive blocking mutex** (`access.rs:68` + `:358`) — 2–4 exclusive acquisitions per
   pipe message, held across a page walk. **Dominant.**
2. **Per-key `WaitQueue` mutex** (`task/futex.rs:38`) — N threads on ONE futex address (a pthread
   barrier/mutex) serialize on that key's queue lock. Valid secondary, worst under thundering-herd
   barriers; Linux serializes per hash-bucket too, so partly inherent.
3. **NOT the futex table lock** — the workflow's futex probe misread; the per-process `FutexTable`
   **is** 64-way sharded (`futex.rs:524`, `FUTEX_SHARDS=64`, `bucket()` splitmix). Distinct futex
   addresses across threads do **not** serialize. That finding is invalid.

## Fix direction (scoped follow-up — high value, high risk, not implemented at session end)
The dominant fix is to stop taking the exclusive blocking aspace lock on the present-page hot path:
1. **Short-circuit `populate_area`** when the range is already fully present (skip the page-table
   walk + `areas.find` under the lock) — shrinks the critical section.
2. **`RwLock<AddrSpace>`**: take a *shared read* lock for the common `can_access_range` +
   already-present case; escalate to *write* only when a page must actually be faulted in. Lets
   concurrent threads validate/copy in parallel — removes the serialization.
3. **Direct HW page-table check** for present+writable and skip the aspace lock entirely on the hot
   copy path (Linux `access_ok` + copy-with-fault-handler).

This is the biggest remaining threaded-IPC lever but touches the correctness-critical user-copy
path — it needs careful design + adversarial review + board A/B, so it is filed as a follow-up
rather than changed blindly. (Note: poll-idle already cut the *wake* half of each contention event
from ~1 ms to µs; this fix removes the *serialization* itself.)

## Attempt 1 — `demand-fault-copy` (drop pre-populate): board-tested, REVERTED (negative)
Tried the lowest-risk option (#1): keep `can_access_range` but drop `populate_area` from the hot
path, relying on demand-fault (Linux demand paging), feature-gated. Board A/B (hackbench):

| | -P g2 / g5 / g10 | -T g2 / g5 / g10 |
|---|---|---|
| baseline | 0.44 / 1.9 / 2.5 | 2.4 / 6.7 / 27 |
| demand-fault | **1.77** / 1.9 / 4.3 | 2.4 / 6.2 / 25.8 |

**Net negative** — it *regressed process mode* (-P g2 0.44→1.77 s) with only a marginal -T gain.
Process mode has no aspace contention (private locks), so pre-populate was cheap; demand-faulting
each first-touch page just adds per-fault lock+handler overhead everywhere. And the marginal -T
improvement confirms the bottleneck is the **serialization (concurrency), not the hold time** —
shortening the critical section doesn't help when all N threads still funnel through the one
exclusive lock. Reverted.

## Conclusion: the fix is concurrency (RwLock), not hold-time or fault-model
Two simpler approaches are now empirically ruled out (adaptive-poll for wake latency; demand-fault
for -T). The only thing that removes the -T serialization is **letting threads validate/copy
concurrently** — i.e. option #2: `RwLock<AddrSpace>` with a shared read lock on the present-page
hot path (`can_access_range` + a new read-only `is_populated` check), escalating to the write lock
only to fault a page in; every other aspace op keeps an exclusive `.write()`. That's a ~98-lock-site
type conversion **plus** a hot-path read/write split — a substantial, correctness-critical refactor
that must be done in its own focused pass with adversarial review + full board A/B, not rushed.
Filed as task #49.
