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
use ax_page_table_multiarch::{PageSize, PageTable64, PagingHandler, PagingMetaData, PagingResult};

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
