//! AArch64 specific page table structures.

use core::arch::asm;

use ax_memory_addr::VirtAddr;
use ax_page_table_entry::aarch64::A64PTE;

use crate::{PageTable64, PageTable64Cursor, PagingMetaData};

/// Metadata of AArch64 page tables.
pub struct A64PagingMetaData;

impl PagingMetaData for A64PagingMetaData {
    const LEVELS: usize = 4;
    const PA_MAX_BITS: usize = 48;
    const VA_MAX_BITS: usize = 48;
    // aarch64 never caches not-present translations, so a fresh (unused → valid)
    // `map` needs no TLB maintenance — like Linux arm64 `set_pte`.
    const NEED_FLUSH_ON_MAP: bool = false;

    type VirtAddr = VirtAddr;

    fn vaddr_is_valid(vaddr: usize) -> bool {
        let top_bits = vaddr >> Self::VA_MAX_BITS;
        top_bits == 0 || top_bits == 0xffff
    }

    #[inline]
    fn flush_tlb(vaddr: Option<VirtAddr>) {
        // The leading `dsb ishst` orders the caller's page-table store (the PTE
        // being invalidated/updated) before the TLBI, so a walker on any core
        // cannot re-cache the stale entry after the invalidation; the trailing
        // `dsb sy; isb` complete the invalidation and synchronize this core. This
        // is the barrier sequence break-before-make relies on (matches Linux
        // `dsb(ishst); tlbi …; dsb(ish); isb`).
        unsafe {
            if let Some(vaddr) = vaddr {
                // TLB Invalidate by VA, All ASID, EL1, Inner Shareable
                const VA_MASK: usize = (1 << 44) - 1; // VA[55:12] => bits[43:0]
                asm!(
                    "dsb ishst; tlbi vaae1is, {}; dsb sy; isb",
                    in(reg) ((vaddr.as_usize() >> 12) & VA_MASK)
                )
            } else {
                // TLB Invalidate, All at stage 1, EL1, Inner Shareable. This is
                // the *broadcast* (`is`) variant, not the local `vmalle1`: the full
                // flush is the cursor's fallback once more than
                // `SMALL_FLUSH_THRESHOLD` VAs are touched (a large unmap/protect),
                // so a local-only flush would leave sibling cores with stale TLB
                // entries — an MT `mprotect`/COW write-protect over many pages
                // could then be bypassed on another core through a stale writable
                // entry. Matches the broadcast `vaae1is` used by the by-VA path.
                asm!("dsb ishst; tlbi vmalle1is; dsb sy; isb")
            }
        }
    }

    // Split of `flush_tlb(Some)` at the completion barrier, for batching many
    // by-VA invalidations behind a single `dsb`. `flush_tlb_nosync` issues the
    // broadcast TLBI (keeping the leading `dsb ishst` that break-before-make
    // relies on) but does NOT wait for completion; `flush_tlb_sync` is the
    // deferred `dsb sy; isb`. A caller MUST run `flush_tlb_sync` before relying
    // on the invalidation — in particular before freeing/reusing a page-table
    // frame whose parent entry was cleared, or a stale walk on another core could
    // read the reused frame.
    #[inline]
    fn flush_tlb_nosync(vaddr: VirtAddr) {
        const VA_MASK: usize = (1 << 44) - 1; // VA[55:12] => bits[43:0]
        unsafe {
            asm!(
                "dsb ishst; tlbi vaae1is, {}",
                in(reg) ((vaddr.as_usize() >> 12) & VA_MASK)
            )
        }
    }

    #[inline]
    fn flush_tlb_sync() {
        unsafe { asm!("dsb sy; isb") }
    }
}

/// AArch64 VMSAv8-64 translation table.
pub type A64PageTable<H> = PageTable64<A64PagingMetaData, A64PTE, H>;
/// AArch64 VMSAv8-64 translation table cursor.
pub type A64PageTableCursor<'a, H> = PageTable64Cursor<'a, A64PagingMetaData, A64PTE, H>;
