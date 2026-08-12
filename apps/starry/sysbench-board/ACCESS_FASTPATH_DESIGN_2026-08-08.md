# User-copy lock-free fast path — design (2026-08-08)

Fixes the `-T` (threaded / `CLONE_VM`) hackbench serialization root-caused in
`THREADED_HACKBENCH_ROOTCAUSE_2026-08-07.md`: every `sys_read`/`sys_write` byte-copy and every
`UserPtr` validation takes the **shared, exclusive, sleeping** `Arc<Mutex<AddrSpace>>` (`access.rs`
`check_region` + `prepare_user_memory`). `CLONE_VM` threads share one aspace → they fully serialize
on that mutex; `fork()` processes have private aspaces → no contention. Hence `-T` is 3–10× slower
than `-P`, the opposite of Linux.

## Why not the literal `RwLock<AddrSpace>` (option #2 in the root-cause doc)

Investigation finding that inverts the earlier recommendation:

1. **`ax_sync::Mutex` is a *sleeping* mutex** (WaitQueue-backed). The write path
   (`populate_area`/`handle_page_fault`) **allocates frames and sleeps while holding it.**
2. **No sleeping/blocking `RwLock` exists in the tree.** The only RwLock is `kspin::RwLock`, a
   *spinlock* — unsafe to hold across a sleep (a writer sleeping while allocating spins readers
   forever; deadlocks outright on 1 CPU). So a literal `RwLock<AddrSpace>` requires **building a new
   sleeping reader/writer lock primitive from scratch** (wait-queue, fairness, ownership tracking,
   lockdep) *plus* converting all 94 `.lock()` sites to `.write()` *plus* the read/write split.
3. **The RwLock has no perf advantage** over the page-table probe here: both let present pages
   validate concurrently, both still serialize actual fault-in (write-lock vs Mutex). The steady
   state hackbench measures (pipe buffers touched once, then reused → all-present) is served
   identically — and the probe is *more* scalable (no shared reader-count cache line bouncing across
   8 cores).

So option #3 (Linux `access_ok` model) dominates on risk with equal perf.

## Approach: lock-free HW page-table permission probe (option #3)

On the user-copy hot path, before taking any lock, probe the **hardware page table** for
present+permission. If every page in the range is present with the required EL0 permission, the
access is legitimate (the user could perform it itself) → **skip the aspace lock entirely** and
copy. If any page misses (cold / COW-read-only / unmapped), fall through to the **existing,
unchanged** slow path (`aspace.lock()` → `can_access_range` → `populate_area`).

### The probe (aarch64)

`AT S1E0R <x>` / `AT S1E0W <x>` asks the MMU to translate a user VA for EL0 read/write under the
*current* translation regime (TTBR0_EL1 = the current thread's aspace — the same table `user_copy`
uses). Result in `PAR_EL1`: bit 0 (`F`) == 0 ⟺ translation succeeded **and** the EL0 access is
permitted. This is exactly the permission the CPU itself enforces, read lock-free.

```
// components/axcpu/src/aarch64/asm.rs, #[cfg(all(feature="uspace", not(feature="arm-el2")))]
at s1e0r|s1e0w, <vaddr>
isb                       // required before reading PAR_EL1 (Linux idiom)
mrs <par>, par_el1
return par & 1 == 0
```

Other arches: `user_access_ok_page` returns `false` → always slow path (correctness preserved, perf
win only on aarch64, which is the board target).

### IRQ safety (why the probe runs IRQs-off)

`PAR_EL1` is a per-CPU scratch shared across contexts. If an IRQ lands between our `AT` and `mrs`
and its handler executes another `AT`, we would read a **stale/foreign** `PAR` — a false positive
could make us skip the lock for an *inaccessible* page (on the `UserPtr` path that becomes a raw
kernel deref of unmapped/foreign memory). So the whole per-range probe runs under
`NoPreemptIrqSave`: no other `AT` can run on this CPU between our `AT` and `mrs`. The window is a
handful of instructions over a **capped** range.

### Range cap

Fast path only applies to ranges ≤ `FASTPATH_MAX_PAGES` (16 pages = 64 KiB) to bound the IRQ-off
window. Pipe IPC messages (~100 B) and syscall-arg structs are far below this; larger transfers take
the slow path where the lock is amortized over a large copy anyway.

## Security argument (the critical part)

Claim: **AT-permitted ⟹ the copy is legitimate.** If the MMU grants EL0 the access, the process's
own page table maps that page with that permission for user mode — the user could perform the exact
access from EL0. The kernel copying to/from a page the user can already read/write introduces no
privilege escalation. This is precisely Linux's model (`copy_to_user` relies on the page table +
fault fixup, not a VMA re-check).

- **Soundness (never accept an illegitimate access):** the fast path accepts only pages the HW
  grants EL0 R/W. A page present+EL0-writable is, by construction, user-writable. ✓
- **Completeness (never wrongly reject a legitimate access):** the fast path only *routes*. Pages it
  rejects (not-present / COW-read-only) fall to the unchanged slow path, which does the full
  `can_access_range` VMA check + `populate_area`. No functional loss. ✓
- **COW correctness:** a COW page is present-but-read-only; `AT S1E0W` on it → `PAR.F=1` → fast path
  misses → slow-path `populate_area` performs the COW copy → retry succeeds. Write never lands on a
  shared COW page. ✓
- **TOCTOU vs concurrent munmap/mprotect:** identical to today. The current code releases the aspace
  lock when `check_region`/`prepare_user_memory` returns; the later deref/copy is *already*
  unprotected and relies on the fault fixup (`user_copy` returns `failed_at`) + the app not
  unmapping its own in-use buffer. The fast path does not weaken this — Linux `access_ok` is
  advisory for the same reason. ✓
- **PTE-more-permissive-than-VMA:** would require a pre-existing kernel bug (axmm keeps PTE ⊆ VMA);
  and even then the user itself could do the access, so no new escalation. ✓

## Feature gating & rollout

- Kernel feature `user-access-fastpath` (default **off**). The probe fn is always compiled (rides
  axcpu `uspace`); only its *use* in `access.rs` is gated.
- Board A/B config `build-aarch64-accessfast-orangepi-5-plus.toml` = ship placement config + the
  feature.
- Promote into the ship placement config **only after** a board A/B shows `-T` approaching `-P`
  without regressing `-P`. Instant revert = drop the feature.

## Adversarial security review (2026-08-08)

A 5-lens adversarial review (soundness/privesc, TOCTOU/concurrency, arch/PAR/IRQ, COW/write-side-
effects, functional/fallback) + an adversarial verify pass ran on the change. Four lenses returned
**clean**; the design's core claims held up (PAN is disabled via `SCTLR_EL1.SPAN=1` so `AT` and the
copy are PAN-independent; TBI0 is off so tagged/high pointers are non-canonical and fault identically
under probe and copy; the old `populate`'s only present-page action, `handle_cow_fault`, fires
exactly when `AT S1E0W` reports `F=1` so the fast-path-accept set and the COW-break set are disjoint).

One **real HIGH finding** (feature-ON build only; never in the default-shipped kernel):

- **Unchecked arithmetic → validation bypass.** `check_region` (the `UserPtr` path) reaches the fast
  path with a fully caller-controlled `start` and, unlike `prepare_user_memory`, **no prior
  `check_access` range bound.** The original helper computed `start + len` / `align_up_4k` /
  `page_end - page_start` unchecked. For `start=0xFFFF_FFFF_FFFF_F800, len=0x900`: `start+len` wraps
  to `0x100`, so `page_start (0xFFFF…F000) > page_end (0x1000)`, the probe loop is **skipped
  entirely**, and the helper returns `true` with **zero probes** → `check_region` returns `Ok` → the
  kernel dereferences a kernel-half VA (panic / corruption). (In a debug/overflow-checks build the
  same input panics at the `start + len`.)

  **Fix (applied):** `user_range_fast_ok` now uses `checked_add` + usize masking, mirroring the slow
  path's `VirtAddrRange::try_from_start_size`, and returns `false` on any overflow. This (a) closes
  the wrap, and (b) guarantees `page_end >= page_start`, so the probe loop **always runs** and the
  `AT` probe rejects any non-present / non-EL0-accessible page (kernel-half addresses included, since
  `AT S1E0R/W` reports a fault for an EL0 access to a TTBR1 page). The fast path now returns `true`
  only after every page has passed the hardware probe.

## Expected result

- `-T` (shared aspace): present pipe buffers validated with **zero shared lock** → threads no longer
  serialize → `-T` should approach `-P`.
- `-P` (private aspace): was already uncontended; fast path replaces one CAS with a cheap HW probe →
  neutral (unlike `demand-fault-copy`, which regressed `-P` by adding faults). This is the key
  difference from the reverted attempt: cold pages still get **bulk-populated once** on the slow
  path; only the *lock* is skipped for warm pages.

## Board A/B result (RK3588, 2026-08-09) — VALIDATED, feature promoted

Clean same-HEAD A/B: placement ship config with vs without `user-access-fastpath` (only that feature
differs). ostool serial flash + `sched-bench.sh` (hackbench `-p -g{2,5,10}` `-P`/`-T`, schbench).
Raw: `schedbench-baselines/starry-{accessfast,placement}-board-2026-08-09.txt`.

| hackbench (Time s, lower=better) | placement (off) | accessfast (on) | speedup |
|---|---|---|---|
| -P g2  | 2.073  | 1.372 | 1.51× |
| -P g5  | 1.815  | 0.902 | 2.01× |
| -P g10 | 6.835  | 3.955 | 1.73× |
| -T g2  | 2.304  | 1.633 | 1.41× |
| -T g5  | 6.535  | 1.045 | **6.25×** |
| -T g10 | 28.541 | 2.784 | **10.25×** |

**The `-T` serialization is removed.** `-T/-P` ratio collapsed from **3.6–4.2×** (placement, g5/g10 —
the "opposite of Linux" pathology) to **~1** (accessfast; g10 even 0.70×) — Linux-like. `-P` *also*
improved 1.5–2× (skipping the lock+`populate_area` walk helps even uncontended). schbench roughly
neutral (m1t4 wakeup p50 9→14 µs, m2t8 5.3→8.7 ms — noise-level; the fast path does not touch the
wake path). Net: unambiguous win, no regression → **enabled in
`build-aarch64-placement-orangepi-5-plus.toml`** (the board ship config).
