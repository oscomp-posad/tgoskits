//! Regression test for the THP split -> unmap -> re-promote defect.
//!
//! Splitting a 2 MiB block to 4 KiB installs an L2 -> L3 table pointer (via
//! `split_huge_page_with`) and 512 leaves. When those leaves are later unmapped
//! the intermediate table becomes empty but was NOT reclaimed, so:
//!   1. it leaked (never freed until the whole page table is dropped), and
//!   2. a subsequent 2 MiB `map` at the same VA hit the stale table and failed
//!      with `AlreadyMapped` (which, in StarryOS, is not `NoMemory`, so the THP
//!      fault-fallback never fired and the process took a SIGSEGV).
//!
//! Uses a mock metadata with a no-op `flush_tlb` so the test does not execute a
//! privileged `tlbi` at EL0 (which would SIGILL on the host). The tracking
//! handler asserts every frame is freed, catching the leak.

use std::{
    alloc::{self, Layout},
    cell::RefCell,
    collections::HashSet,
    marker::PhantomData,
};

use ax_memory_addr::{PhysAddr, VirtAddr};
use ax_page_table_entry::{MappingFlags, aarch64::A64PTE};
use ax_page_table_multiarch::{
    PageSize, PageTable64, PagingError, PagingHandler, PagingMetaData, PagingResult,
};

const PAGE_LAYOUT: Layout = unsafe { Layout::from_size_align_unchecked(4096, 4096) };

thread_local! {
    static LIVE: RefCell<HashSet<usize>> = RefCell::default();
}

/// Tracks every table frame the page table allocates so the test can assert
/// there are no leaks.
struct TrackHandler<M: PagingMetaData>(PhantomData<M>);

impl<M: PagingMetaData> PagingHandler for TrackHandler<M> {
    fn alloc_frame() -> Option<PhysAddr> {
        let ptr = unsafe { alloc::alloc(PAGE_LAYOUT) } as usize;
        assert!(ptr <= M::PA_MAX_ADDR);
        LIVE.with_borrow_mut(|it| it.insert(ptr));
        Some(PhysAddr::from_usize(ptr))
    }
    fn alloc_frames(num: usize, align: usize) -> Option<PhysAddr> {
        assert_eq!(num, 1);
        let _ = align;
        Self::alloc_frame()
    }
    fn dealloc_frame(paddr: PhysAddr) {
        let ptr = paddr.as_usize();
        LIVE.with_borrow_mut(|it| assert!(it.remove(&ptr), "double/foreign free"));
        unsafe { alloc::dealloc(ptr as _, PAGE_LAYOUT) };
    }
    fn dealloc_frames(paddr: PhysAddr, num: usize) {
        assert_eq!(num, 1);
        Self::dealloc_frame(paddr);
    }
    fn phys_to_virt(paddr: PhysAddr) -> VirtAddr {
        VirtAddr::from_usize(paddr.as_usize())
    }
}

/// aarch64-shaped metadata whose `flush_tlb` is a no-op (a real `tlbi` would
/// SIGILL at EL0 on the host).
struct MockMeta;
impl PagingMetaData for MockMeta {
    const LEVELS: usize = 4;
    const PA_MAX_BITS: usize = 48;
    const VA_MAX_BITS: usize = 48;
    const NEED_FLUSH_ON_MAP: bool = false;
    type VirtAddr = VirtAddr;
    fn vaddr_is_valid(vaddr: usize) -> bool {
        let top = vaddr >> Self::VA_MAX_BITS;
        top == 0 || top == 0xffff
    }
    fn flush_tlb(_vaddr: Option<VirtAddr>) {}
}

type Pt = PageTable64<MockMeta, A64PTE, TrackHandler<MockMeta>>;

const HUGE_2M: usize = 2 * 1024 * 1024;
const PG: usize = 4096;
const RW: MappingFlags = MappingFlags::from_bits_truncate(
    MappingFlags::READ.bits() | MappingFlags::WRITE.bits(),
);

#[test]
fn thp_split_unmap_remap_reclaims_table_and_succeeds() -> PagingResult<()> {
    LIVE.with_borrow_mut(|it| it.clear());

    // A 2 MiB-aligned virtual and (dummy, never dereferenced) physical address.
    let va = VirtAddr::from_usize(0x40_0000);
    let pa = PhysAddr::from_usize(0x1000_0000);

    let mut pt = Pt::try_new().unwrap();

    // 1) Map the 2 MiB block.
    pt.cursor().map(va, pa, PageSize::Size2M, RW)?;

    // 2) Split it: reserve the leaf table, splice it in (break-before-make),
    //    then install the 512 leaves — the same sequence the kernel THP split
    //    performs.
    let table = pt.alloc_intermediate_table()?;
    {
        let mut c = pt.cursor();
        c.split_huge_page_with(va, table)?;
        for i in 0..(HUGE_2M / PG) {
            c.map(
                va + i * PG,
                PhysAddr::from_usize(pa.as_usize() + i * PG),
                PageSize::Size4K,
                RW,
            )?;
        }
    }

    // 3) Unmap all 512 leaves (what munmap of the split region does).
    {
        let mut c = pt.cursor();
        for i in 0..(HUGE_2M / PG) {
            c.unmap(va + i * PG)?;
        }
    }

    // 4) Re-promote: map a fresh 2 MiB block at the same VA. Before the fix this
    //    returned Err(AlreadyMapped) because the empty L3 table still occupied
    //    the L2 slot. It must now succeed (the empty table is reclaimed).
    let pa2 = PhysAddr::from_usize(0x2000_0000);
    pt.cursor()
        .map(va, pa2, PageSize::Size2M, RW)
        .expect("2 MiB re-map over a previously-split (now empty) slot must succeed");

    // The re-mapped 2 MiB block resolves to the new frame.
    let (got, _flags, sz) = pt.query(va)?;
    assert_eq!(sz, PageSize::Size2M);
    assert_eq!(got.as_usize(), pa2.as_usize());

    // 5) Tear down and assert no page-table frame leaked (the split's L3 table
    //    must have been reclaimed, not stranded).
    pt.cursor().unmap(va)?;
    drop(pt);
    LIVE.with_borrow(|it| assert!(it.is_empty(), "leaked {} page-table frame(s)", it.len()));

    Ok(())
}

// A not-present huge block (e.g. after `mprotect(PROT_NONE)`) reads
// `is_huge == false` on aarch64/riscv. Its data frame must NOT be misread as a
// page table and freed by the reclaim or the huge-map path. `TrackHandler`
// panics on a foreign free, so a regression here fails loudly.
#[test]
fn not_present_huge_block_is_not_reclaimed() -> PagingResult<()> {
    LIVE.with_borrow_mut(|it| it.clear());

    // A zeroed, 2 MiB-aligned data frame — reading it as a page table would find
    // every entry "unused" (the trap that would make `table_all_unused` true).
    let layout = Layout::from_size_align(HUGE_2M, HUGE_2M).unwrap();
    let d = unsafe { alloc::alloc_zeroed(layout) } as usize;
    assert!(d != 0);

    let va = VirtAddr::from_usize(0x40_0000);
    let mut pt = Pt::try_new().unwrap();

    // Present 2 MiB block backed by D, then drop it to no-access -> a non-present
    // huge block (VALID clear => is_huge()==false).
    pt.cursor().map(va, PhysAddr::from_usize(d), PageSize::Size2M, RW)?;
    pt.cursor().protect(va, MappingFlags::empty())?;

    // Neither path may touch D. If either misread D as a table and freed it,
    // TrackHandler would panic (D was never allocated through it).
    pt.reclaim_empty_tables(va, HUGE_2M);
    let remap = pt
        .cursor()
        .map(va, PhysAddr::from_usize(0x8000_0000), PageSize::Size2M, RW);
    assert_eq!(remap, Err(PagingError::AlreadyMapped));

    // Teardown must also leave D untouched: `dealloc_tree` descends via
    // `next_table`, which (using `is_table()`) refuses to walk into the
    // not-present huge block. Dropping `pt` frees its own tables but never D — a
    // foreign free would panic in `TrackHandler`.
    drop(pt);
    LIVE.with_borrow(|it| assert!(it.is_empty(), "leaked {} table frame(s)", it.len()));
    unsafe { alloc::dealloc(d as *mut u8, layout) };
    Ok(())
}

// Unmapping a not-present huge block (post `mprotect(PROT_NONE)`) must hand its
// data frame back to the caller — otherwise the frame leaks (the mm layer frees
// exactly what `unmap` returns; a `MappedToHugePage`/`NotMapped` error frees
// nothing). This is the leak the `is_table` fix would otherwise trade the UAF
// for.
#[test]
fn unmap_not_present_huge_block_returns_its_frame() -> PagingResult<()> {
    LIVE.with_borrow_mut(|it| it.clear());

    let va = VirtAddr::from_usize(0x40_0000);
    let d = PhysAddr::from_usize(0x1000_0000); // dummy data frame, never dereferenced
    let mut pt = Pt::try_new().unwrap();

    // Present 2 MiB block backed by D, then drop to no-access -> not-present huge
    // block (VALID clear, frame retained).
    pt.cursor().map(va, d, PageSize::Size2M, RW)?;
    pt.cursor().protect(va, MappingFlags::empty())?;

    // Before the fix `unmap` returned Err(MappedToHugePage) and D leaked. It must
    // now return D + its size so the caller can free it.
    let (paddr, _flags, size) = pt
        .cursor()
        .unmap(va)
        .expect("unmap of a not-present huge block must return its frame, not error");
    assert_eq!(size, PageSize::Size2M);
    assert_eq!(paddr.as_usize(), d.as_usize());

    // The slot is now free: a fresh 2 MiB map at the same VA succeeds, and the
    // page table drops with no stranded table frames.
    pt.cursor()
        .map(va, PhysAddr::from_usize(0x2000_0000), PageSize::Size2M, RW)?;
    pt.cursor().unmap(va)?;
    drop(pt);
    LIVE.with_borrow(|it| assert!(it.is_empty(), "leaked {} table frame(s)", it.len()));

    Ok(())
}

// The 4 KiB analogue of the huge case: a not-present 4 KiB leaf (post
// `mprotect(PROT_NONE)`) still owns its frame, which `unmap` must return rather
// than clearing the PTE and reporting `NotMapped` (which would leak the frame).
// A genuinely unused slot must still report `NotMapped`.
#[test]
fn unmap_not_present_4k_leaf_returns_its_frame() -> PagingResult<()> {
    LIVE.with_borrow_mut(|it| it.clear());

    let va = VirtAddr::from_usize(0x40_0000);
    let d = PhysAddr::from_usize(0x1000_0000); // dummy data frame, never dereferenced
    let mut pt = Pt::try_new().unwrap();

    pt.cursor().map(va, d, PageSize::Size4K, RW)?;
    pt.cursor().protect(va, MappingFlags::empty())?; // VALID clear, frame retained

    let (paddr, _flags, size) = pt
        .cursor()
        .unmap(va)
        .expect("unmap of a not-present 4K leaf must return its frame, not error");
    assert_eq!(size, PageSize::Size4K);
    assert_eq!(paddr.as_usize(), d.as_usize());

    // The slot is now unused: a second unmap reports NotMapped (nothing to free).
    assert_eq!(pt.cursor().unmap(va), Err(PagingError::NotMapped));

    drop(pt);
    LIVE.with_borrow(|it| assert!(it.is_empty(), "leaked {} table frame(s)", it.len()));

    Ok(())
}

// An on-demand lazy page mapped as `paddr 0, empty flags` (the ArceOS/Axvisor
// alloc backend's non-populate path) is not-present and, on aarch64/riscv, NOT
// `is_unused()` — empty flags set present-independent bits (aarch64 AF|NON_BLOCK
// => raw 0x402). It owns no real frame (frame 0 is never allocatable), so unmap
// must report `NotMapped`, not return `Ok((0, ..))` — the latter would make the
// alloc backend `dealloc_frame(0)` and corrupt the allocator at teardown.
#[test]
fn unmap_lazy_zero_paddr_entry_frees_nothing() -> PagingResult<()> {
    LIVE.with_borrow_mut(|it| it.clear());

    let va = VirtAddr::from_usize(0x40_0000);
    let mut pt = Pt::try_new().unwrap();
    // Mirrors alloc.rs `map_region(.., |_| 0.into(), .., empty, populate=false)`.
    pt.cursor()
        .map(va, PhysAddr::from_usize(0), PageSize::Size4K, MappingFlags::empty())?;

    // Must be reported unmapped (no frame handed back), and the slot torn down.
    assert_eq!(pt.cursor().unmap(va), Err(PagingError::NotMapped));

    drop(pt);
    LIVE.with_borrow(|it| assert!(it.is_empty(), "leaked {} table frame(s)", it.len()));

    Ok(())
}

// Reclaiming a large range frees many intermediate tables through the batched
// TLB-flush path (buffer freed frames, drain-when-full behind one sync, then a
// tail drain). `TrackHandler` panics on any double/foreign free, so this pins the
// batch's frame accounting across the 32-frame `RECLAIM_TLB_BATCH` boundary.
// Counts 1/32/33/200 exercise tail-only, exactly-full, first-drain+tail, and
// multiple-drains+tail. MockMeta's no-op flush also exercises the default
// (non-aarch64) `flush_tlb_nosync`/`flush_tlb_sync` path.
#[test]
fn reclaim_batches_many_tables_no_double_free() -> PagingResult<()> {
    for &n in &[1usize, 32, 33, 200] {
        LIVE.with_borrow_mut(|it| it.clear());
        let mut pt = Pt::try_new().unwrap();
        let base = 0x40_0000usize; // 2 MiB-aligned

        // One 4 KiB page in each of n distinct 2 MiB spans => n stranded L3 tables
        // (sharing one L2/L1), which the reclaim must free through the batch.
        for i in 0..n {
            let va = VirtAddr::from_usize(base + i * HUGE_2M);
            pt.cursor().map(
                va,
                PhysAddr::from_usize(0x1000_0000 + i * PG),
                PageSize::Size4K,
                RW,
            )?;
        }
        for i in 0..n {
            let va = VirtAddr::from_usize(base + i * HUGE_2M);
            pt.cursor().unmap(va)?;
        }

        let before = LIVE.with_borrow(|it| it.len());
        pt.reclaim_empty_tables(VirtAddr::from_usize(base), n * HUGE_2M);
        let after = LIVE.with_borrow(|it| it.len());
        assert!(
            after < before,
            "n={n}: reclaim should free stranded tables {before} -> {after}"
        );

        // Teardown must not double-free anything the batch already freed.
        drop(pt);
        LIVE.with_borrow(|it| assert!(it.is_empty(), "n={n}: leaked {} frame(s)", it.len()));
    }
    Ok(())
}

#[test]
fn thp_split_unmap_reclaims_empty_table_on_unmap() -> PagingResult<()> {
    LIVE.with_borrow_mut(|it| it.clear());

    let va = VirtAddr::from_usize(0x40_0000);
    let pa = PhysAddr::from_usize(0x1000_0000);
    let mut pt = Pt::try_new().unwrap();

    // Map + split (installs an L3 table), then unmap every leaf — which strands
    // the now-empty L3 table (the leak this reclaim closes).
    pt.cursor().map(va, pa, PageSize::Size2M, RW)?;
    let table = pt.alloc_intermediate_table()?;
    {
        let mut c = pt.cursor();
        c.split_huge_page_with(va, table)?;
        for i in 0..(HUGE_2M / PG) {
            c.map(
                va + i * PG,
                PhysAddr::from_usize(pa.as_usize() + i * PG),
                PageSize::Size4K,
                RW,
            )?;
        }
    }
    {
        let mut c = pt.cursor();
        for i in 0..(HUGE_2M / PG) {
            c.unmap(va + i * PG)?;
        }
    }

    // Reclaim frees the empty L3 table and cascades up the L2/L1 that held only
    // it, leaving just the root live BEFORE teardown (the leak is closed eagerly,
    // not deferred to `Drop`).
    let before = LIVE.with_borrow(|it| it.len());
    pt.reclaim_empty_tables(va, HUGE_2M);
    let after = LIVE.with_borrow(|it| it.len());
    assert!(after < before, "reclaim should free stranded table(s): {before} -> {after}");
    assert_eq!(after, 1, "only the root table should remain live after reclaim");

    drop(pt);
    LIVE.with_borrow(|it| assert!(it.is_empty(), "leaked {} page-table frame(s)", it.len()));

    Ok(())
}
