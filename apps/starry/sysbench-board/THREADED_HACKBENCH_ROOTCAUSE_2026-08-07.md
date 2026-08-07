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
