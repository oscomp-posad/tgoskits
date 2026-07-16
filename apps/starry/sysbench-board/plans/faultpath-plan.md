<!-- from workflow wv18ljyfc; line numbers verify against tree before coding -->

# Speeding up the StarryOS first-touch / page-fault path (RK3588)

## Cost model — CONFIRMED against current branch code (corrects the scout)

Verified in `feat/rknn-profiling-harness`:

- **Fault-around ALREADY exists.** `AddrSpace::handle_page_fault` (`os/StarryOS/kernel/src/mm/aspace/mod.rs:527-575`) builds a **32-page** forward window (`READAHEAD_PAGES=32`, :543) for 4K backends and calls `backend.populate(range, …, &mut self.pt.cursor())` once. So a 256 MB memset is ~2048 faults, **not 65 536**. The scout's "one page per fault" is wrong — **the 11× is per-page work, not fault count.** Every phase below attacks per-page cost.
- **Whole 32-page populate runs under the per-process aspace `Mutex`** (`access.rs:368` `aspace_arc.lock().handle_page_fault(...)`). Single-thread membw doesn't contend; multi-thread faulting in one process fully serializes here.
- **Per-page work paid ~65 536× for 256 MB**, ranked by excess-over-Linux:
  1. **Broadcast TLBI per fresh page (pure waste).** `PageTableCursor::map` (`memory/page_table_multiarch/src/bits64.rs:664-671`) unconditionally `self.push(vaddr)` for every fresh map. The cursor is created per fault and `READAHEAD_PAGES=32 == SMALL_FLUSH_THRESHOLD=32` (`lib.rs:166`), so the 32-entry ArrayVec fills *exactly* and never overflows to the single `Full` flush → on drop it emits **32 individual `tlbi vaae1is; dsb sy; isb`** (`arch/aarch64.rs:31`) = inner-shareable **broadcast to all 8 cores + full-system barrier, per page**. A fresh not-present→present PTE needs **zero** TLB maintenance on aarch64 (no stale entry can be cached; Linux `set_pte` doesn't flush).
  2. **Global `FRAME_TABLE` per page.** `alloc_new_frame` (`cow.rs:112-116`) → `FRAME_TABLE.lock().init_frame` where `FRAME_TABLE` is one global `SpinNoIrq<BTreeMap<PhysAddr, Arc<SpinNoIrq<FrameRefCnt>>>>` (`cow.rs:79`). `init_frame` (`cow.rs:59-68`) = **one heap `Arc::new` + one O(log N) BTreeMap insert under a single global lock**, per page. Linux uses an O(1) `struct page` refcount — no global lock, no heap alloc, no tree.
  3. **Two global buddy locks per page.** `alloc_frame` (`backend/mod.rs:34-48`) → `global_allocator().alloc_pages(1, 4096, VirtMem)` which takes `self.inner.lock()` (`buddy_slab.rs:199`) **and** `self.usages.lock()` (:221) every call. Page allocation bypasses the per-CPU slab (`def_percpu`, :30, serves only byte allocs). Linux uses per-CPU pcp lists.
  4. **Generic memset zeroing, no DC ZVA.** `alloc_frame` zeroes via `core::ptr::write_bytes` (`backend/mod.rs:43`); no aarch64 `dc zva` clear-page exists. Linux arm64 `clear_page` uses `dc zva` (~3-5× faster, avoids read-for-ownership).

- **Fixed per-fault overhead (trap, aspace lock, `areas.find`) is already amortized 32× by fault-around** — it is *not* the lever.
- **madvise prefault is a silent no-op.** `sys_madvise` (`mmap.rs:838-889`) accepts `MADV_WILLNEED`/`MADV_POPULATE_READ`/`MADV_POPULATE_WRITE` as valid (:845) but the action match only handles `DONTNEED`/`FREE`/`DONTNEED_LOCKED` (:884). `MAP_POPULATE` *is* honored (:485 → `populate_area`); `populate_area` (`mod.rs:190`) is the ready primitive.
- **Huge pages: whole lower stack already supports 2 MB blocks** (`CowBackend.size: PageSize`, `pt.map(vaddr, frame, self.size)`, A64PTE block descriptors, contiguous `alloc_frame`). Gap is *policy* + a *huge→4K split* primitive.

---

## PHASE 1 — biggest easy win: stop flushing the TLB on fresh maps (arch-gated)

**Change site:** `PageTableCursor::map` in `memory/page_table_multiarch/src/bits64.rs:664-671`, plus a new associated const on `PagingMetaData`.

**Mechanism.** `map()` only ever installs into an `is_unused()` entry — it returns `AlreadyMapped` otherwise (`bits64.rs:667`). So **every** successful `map()` is a fresh not-present→present transition, which requires no TLB maintenance on aarch64/x86_64. Add `const NEED_FLUSH_ON_MAP: bool` to `PagingMetaData` (default `true` for safety), set it `false` for aarch64 (`A64PagingMetaData`) and x86_64, and gate the `self.push(vaddr)` in `map()` on it. This removes the ~65 536 broadcast `tlbi vaae1is; dsb sy; isb` sequences from a 256 MB first-touch **outright**. It fixes both the demand-fault path *and* `populate_area`/`MAP_POPULATE`, commits no memory, and can't game the metric (fault count unchanged).

Keep it **per-arch**: RISC-V permits negative caching of invalid PTEs (an `sfence.vma` may be required after invalid→valid), and LoongArch is unverified — leave `NEED_FLUSH_ON_MAP=true` for both.

**Correctness gates.**
- Only `map()` (is_unused → valid) skips the flush. `remap`/`protect`/`unmap` change or remove *valid* entries (break-before-make) and MUST keep flushing — do not touch them.
- Within one cursor, an `unmap` then `map` of the same vaddr: `unmap` still pushed the vaddr (or set `Full`), so the drop-flush still covers it. `map` never silently replaces a valid entry (returns `AlreadyMapped`), so no stale entry survives unflushed.
- The `populate_area` file-cache eviction path (`mod.rs:211-218`) unmaps evicted frames via `unmap` (valid→invalid) — unaffected, still flushes.
- Shared crate: SMP-correctness review required; the aarch64 guarantee (a translation that faulted is not cached, on any core) is the correctness boundary.
- Tests: (a) fault a page, read-back; (b) fork→COW write still correct (uses `remap`, unchanged); (c) `mmap`+immediate read across the window (no phantom re-fault); (d) two threads fault adjacent pages concurrently → no lost mapping.

**Expected effect / verification.** membw `firsttouch_s` drops by the TLBI/barrier share of the ~0.8-1.3 s (reasoned, not yet profiled — the harness rung is the arbiter); `sysbench memory @1KB` rises off 42 MiB/s; `/proc/vmstat pgfault` unchanged. **= snapshot rung.**

---

## PHASE 2 — remove the two per-page global-lock taxes + wire madvise prefault

**2a — Replace `FRAME_TABLE` (Arc+BTreeMap under one lock) with a flat per-PFN atomic refcount.** Site: `cow.rs:23-79` (+ `init_frame`/`get_frame_ref`/`drop_frame` callers). Allocate one `[AtomicU16]` sized to buddy-managed RAM, indexed by `pfn = (paddr - phys_base) >> 12`. `init_frame` = one atomic store; inc/dec = `fetch_add`/`fetch_sub`. Removes, per page: one global lock, one heap `Arc` alloc, one O(log N) tree insert/remove — a Starry-only tax Linux never pays. `AtomicU16` also lifts today's `u8` max-255 sharer cap (`FrameRefCnt(u8)`, `cow.rs:23`).
  - *Gates:* preserve the drop→dealloc ordering guard (`cow.rs:31-37`): the dec that observes `1→0` owns the dealloc (via `fetch_sub`), so exactly one dealloc runs and no other thread can re-init the frame mid-drop. `clone_map` fork inc becomes `fetch_add` keeping the overflow check. Bounds-check the PFN (only anon/CoW buddy frames are tracked). This is CoW-correctness-critical → run the full fork/COW/munmap test matrix.

**2b — Batch allocation cost across the 32-page window.** Site: anon `NotMapped` run in `CowBackend::populate` (`cow.rs:354-369`) and `backend/mod.rs:34-48`. Safe core: hoist `usages.lock()` (`buddy_slab.rs:221`) to once-per-window and zero the run in one `write_bytes` instead of 32 small ones. Fuller win (mirror the existing file-side `alloc_file_run`, `cow.rs:165`): one contiguous `alloc_pages(run_len)` + one big zero + per-4K map/refcount — **only if** the buddy can free the block per-4K (munmap/discard are 4K-granular); otherwise keep per-4K `alloc_pages(1)` so dealloc stays symmetric. Requires 4K fallback under fragmentation.
  - *Gates:* freeing stays 4K-granular; if a contiguous block is allocated it must be splittable into independently-freeable 4K frames, else keep per-4K alloc. Fall back to per-page on order-N buddy failure.

**2c — Wire madvise prefault (RKNN-relevant, ~5 lines).** Site: `mmap.rs:884` match arm. Route `MADV_POPULATE_WRITE`→`aspace.populate_area(addr, len, MappingFlags::WRITE)`, `MADV_POPULATE_READ`/`MADV_WILLNEED`→`READ` (best-effort). The primitive already exists (`populate_area`, used by mlock at `mmap.rs:990`). Lets the RKNN pipeline explicitly pre-commit large DMA/model/scratch buffers Linux-accurately.
  - *Gates:* `POPULATE_WRITE` on a read-only VMA → `EINVAL` (Linux); `WILLNEED` ignores per-fragment errors.

**Verification.** `firsttouch_s` and `sysbench @1KB` improve further; multi-threaded first-touch now scales (global `FRAME_TABLE` lock + one buddy lock gone from the hot path). **= rung.**

---

## PHASE 3 — optional, biggest for large allocs: 2 MB THP-lite + DC ZVA

**Mechanism.** Transparently back large aligned private-anon areas with 2 MB blocks. One 2 MB fault → 512 pages resident, 512× fewer faults, 512× fewer PT leaf writes, one TLB block entry. This is what gets Linux to 0.086 s and is the biggest lever for the 128 MB membw buffers *and* RKNN's large buffers. Lower stack is already complete (`CowBackend.size`, `pt.map(self.size)`, A64PTE blocks, contiguous `alloc_frame`); template is the explicit `MAP_HUGETLB` path (`mmap.rs:179-185`).

**Required new work.**
- **huge→4K split primitive (the blocker).** `pages_in(range, Size2M)` (`DynPageIter`, `memory/memory_addr/src/iter.rs`) requires both ends 2 MB-aligned, so any sub-2 MB `munmap`/`mprotect`/`madvise` on a promoted area errors (`InvalidInput`→`BadState`) or the PT `unmap_region` assert panics/clears the whole block. Implement `split_huge` (re-map the 2 MB block as 512 identical 4K PTEs, downgrade area/backend to `Size4K`) and call it before any sub-2 MB op. **Land this before any promotion.**
- **Policy in `sys_mmap` anon path** (`mmap.rs:531-544`): promote only when private && anon && writable && size ≥ 2 MB && `!MADV_NOHUGEPAGE` && `PR_SET_THP_DISABLE` unset (gate already exists, `task/mod.rs`). Carve into `[4K head][2M body][4K tail]` — only the 2 MB-aligned interior gets a `Size2M` backend. **Do NOT round whole areas up to 2 MB** (breaks `/proc/maps` + mmap length).
- **Graceful 4K fallback** in `alloc_frame(Size2M)` and `CowBackend::populate` when the order-9 buddy block is unavailable (fragmentation). **Skip 1 GB entirely** (order-18 unreliable).
- **DC ZVA clear-page** (aarch64): read `DCZID_EL0` block size at init, use `dc zva` in `alloc_frame`'s zero path (`backend/mod.rs:43`). ~3-5× faster; secondary once fault count drops 512×, but it's the zeroing floor and helps the 4K path too.

**Correctness gates.** split lands first (else partial munmap/mprotect/mremap/`MADV_DONTNEED` panics); `handle_cow_fault` copies whole `self.size` (2 MB) on first write — split-on-COW-write to avoid a 2 MB copy per child byte-write; `discard_range` 4K sub-range on a 2 MB area currently rounds inward → split first or it no-ops (regression); RSS/VA inflation for sparse touch → gate on size+alignment, off for `MAP_NORESERVE`; verify `DCZID_EL0` block size at runtime (wrong size corrupts adjacent memory).

**Verification.** `firsttouch_s` toward Linux 0.086 s (512× fault reduction); `sysbench @1KB` up sharply; `/proc/vmstat pgfault` ~512× lower for large anon. **= rung.**

---

## FIRST PR scope

**Phase 1 only:** the arch-gated "no TLB flush on fresh map" — add `NEED_FLUSH_ON_MAP` to `PagingMetaData` (false for aarch64/x86_64, true for riscv64/loongarch), gate `self.push` in `PageTableCursor::map` (`bits64.rs:664-671`), plus the fork/COW + concurrent-fault regression tests and a `firsttouch_s` snapshot rung. Smallest diff, no memory committed, no metric-gaming, and it improves ArceOS/Axvisor too since it's in the shared `page_table_multiarch` crate.

Optionally fold in **Phase 2c** (madvise `POPULATE_WRITE` wiring, ~5 lines) — independent, low-risk, and the most directly RKNN-useful discrete change.

## Why this ordering also front-loads the RKNN pipeline win
Every large RKNN buffer (camera frames, tensor/model/scratch) is first-touched once and today pays the same per-page TLBI storm + global `FRAME_TABLE` lock. **Phase 1** removes the TLBI storm from every buffer bring-up implicitly; **Phase 2a** removes the global-lock serialization so concurrent worker threads bringing up buffers stop serializing on `FRAME_TABLE`; **Phase 2c** lets the pipeline explicitly `madvise(POPULATE_WRITE)` its buffers; **Phase 3** pre-commits a big contiguous buffer in 512× fewer faults with 2 MB TLB coverage — the ideal shape for DMA/model buffers.

## Caveat carried from the investigation
All magnitudes are static-reasoned, not yet profiled per-step. Each phase is gated on its own harness rung (`firsttouch_s`, `sysbench memory @1KB`); treat the rung, not the estimate, as the arbiter. The harness (`apps/starry/sysbench-board/harness/`) is not on this branch — it lives on `worktree-sysbench` (parent task #16 is wiring it), so Phase 1's rung depends on that landing first.

Key files: `os/StarryOS/kernel/src/mm/aspace/mod.rs`, `os/StarryOS/kernel/src/mm/aspace/backend/cow.rs`, `os/StarryOS/kernel/src/mm/aspace/backend/mod.rs`, `os/StarryOS/kernel/src/mm/access.rs`, `os/StarryOS/kernel/src/syscall/mm/mmap.rs`, `memory/page_table_multiarch/src/bits64.rs`, `memory/page_table_multiarch/src/lib.rs`, `memory/page_table_multiarch/src/arch/aarch64.rs`, `os/arceos/modules/axalloc/src/buddy_slab.rs`.
