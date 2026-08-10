use core::{marker::PhantomData, ops::Deref};

use arrayvec::ArrayVec;
use ax_memory_addr::{MemoryAddr, PAGE_SIZE_4K, PhysAddr};

use crate::{
    GenericPTE, MappingFlags, PageSize, PagingError, PagingHandler, PagingMetaData, PagingResult,
    TlbFlusher,
};

const ENTRY_COUNT: usize = 512;

const fn p4_index(vaddr: usize) -> usize {
    (vaddr >> (12 + 27)) & (ENTRY_COUNT - 1)
}

const fn p3_index(vaddr: usize) -> usize {
    (vaddr >> (12 + 18)) & (ENTRY_COUNT - 1)
}

const fn p2_index(vaddr: usize) -> usize {
    (vaddr >> (12 + 9)) & (ENTRY_COUNT - 1)
}

const fn p1_index(vaddr: usize) -> usize {
    (vaddr >> 12) & (ENTRY_COUNT - 1)
}

/// A generic page table struct for 64-bit platform.
///
/// It also tracks all intermediate level tables. They will be deallocated
/// When the [`PageTable64`] itself is dropped.
pub struct PageTable64<M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> {
    root_paddr: PhysAddr,
    root_frames: usize,
    root_allocated_with_frames: bool,
    #[cfg(feature = "copy-from")]
    borrowed_entries: bitmaps::Bitmap<ENTRY_COUNT>,
    _phantom: PhantomData<(M, PTE, H)>,
}

impl<M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> PageTable64<M, PTE, H> {
    /// Creates a new page table instance or returns the error.
    ///
    /// It will allocate a new page for the root page table.
    pub fn try_new() -> PagingResult<Self> {
        let root_paddr = Self::alloc_table()?;
        Ok(Self {
            root_paddr,
            root_frames: 1,
            root_allocated_with_frames: false,
            #[cfg(feature = "copy-from")]
            borrowed_entries: bitmaps::Bitmap::new(),
            _phantom: PhantomData,
        })
    }

    /// Creates a new page table with a root table allocation satisfying the
    /// given frame count and alignment.
    ///
    /// This is useful for hardware page table formats whose root table has
    /// stricter allocation requirements than intermediate tables, while the
    /// common 64-bit page table walker still uses 4K tables internally.
    pub fn try_new_with_root(root_frames: usize, root_align: usize) -> PagingResult<Self> {
        if root_frames == 1 && root_align == PAGE_SIZE_4K {
            return Self::try_new();
        }
        let root_paddr = Self::alloc_root_table(root_frames, root_align)?;
        Ok(Self {
            root_paddr,
            root_frames,
            root_allocated_with_frames: true,
            #[cfg(feature = "copy-from")]
            borrowed_entries: bitmaps::Bitmap::new(),
            _phantom: PhantomData,
        })
    }

    /// Returns the physical address of the root page table.
    pub const fn root_paddr(&self) -> PhysAddr {
        self.root_paddr
    }

    /// Queries the result of the mapping starting at `vaddr`.
    ///
    /// Returns the physical address of the target frame, mapping flags, and
    /// the page size.
    ///
    /// Returns [`Err(PagingError::NotMapped)`](PagingError::NotMapped) if the
    /// mapping is not present.
    pub fn query(&self, vaddr: M::VirtAddr) -> PagingResult<(PhysAddr, MappingFlags, PageSize)> {
        let (entry, size) = self.get_entry(vaddr)?;
        if !entry.is_present() {
            return Err(PagingError::NotMapped);
        }
        let off = size.align_offset(vaddr.into());
        Ok((entry.paddr().add(off), entry.flags(), size))
    }

    /// Walk the page table recursively.
    ///
    /// When reaching a page table entry, call `pre_func` and `post_func` on the
    /// entry if they are provided. The max number of enumerations in one table
    /// is limited by `limit`. `pre_func` and `post_func` are called before and
    /// after recursively walking the page table.
    ///
    /// The arguments of `*_func` are:
    /// - Current level (starts with `0`): `usize`
    /// - The index of the entry in the current-level table: `usize`
    /// - The virtual address that is mapped to the entry: `M::VirtAddr`
    /// - The reference of the entry: [`&PTE`](GenericPTE)
    pub fn walk<F>(&self, limit: usize, pre_func: Option<&F>, post_func: Option<&F>)
    where
        F: Fn(usize, usize, M::VirtAddr, &PTE),
    {
        self.walk_recursive(
            self.table_of(self.root_paddr()),
            0,
            0.into(),
            limit,
            pre_func,
            post_func,
        )
    }

    /// Gets a cursor to modify the page table.
    ///
    /// The TLB will be flushed automatically when the cursor is dropped.
    pub fn cursor(&mut self) -> PageTable64Cursor<'_, M, PTE, H> {
        PageTable64Cursor::new(self)
    }

    /// Allocates a fresh, zeroed intermediate (non-leaf) page-table frame for a
    /// caller that will later splice it in with
    /// [`PageTable64Cursor::split_huge_page_with`].
    ///
    /// Reserving the table up front lets a huge-page split be *committed* with no
    /// allocation — i.e. atomically against out-of-memory: if this reservation
    /// fails the caller can abort before mutating any mapping, and once it
    /// succeeds the commit cannot fail for lack of memory partway through. A
    /// reserved frame that ends up unused must be returned with
    /// [`dealloc_intermediate_table`](Self::dealloc_intermediate_table).
    pub fn alloc_intermediate_table(&self) -> PagingResult<PhysAddr> {
        Self::alloc_table()
    }

    /// Frees a frame obtained from
    /// [`alloc_intermediate_table`](Self::alloc_intermediate_table) that was not
    /// consumed by a split (rollback path).
    pub fn dealloc_intermediate_table(&self, paddr: PhysAddr) {
        H::dealloc_frame(paddr);
    }
}

// Private implements.
impl<M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> PageTable64<M, PTE, H> {
    fn alloc_table() -> PagingResult<PhysAddr> {
        if let Some(paddr) = H::alloc_frame() {
            let ptr = H::phys_to_virt(paddr).as_mut_ptr();
            unsafe { core::ptr::write_bytes(ptr, 0, PAGE_SIZE_4K) };
            Ok(paddr)
        } else {
            Err(PagingError::NoMemory)
        }
    }

    fn alloc_root_table(root_frames: usize, root_align: usize) -> PagingResult<PhysAddr> {
        if root_frames == 0 || root_align < PAGE_SIZE_4K || !root_align.is_power_of_two() {
            return Err(PagingError::NoMemory);
        }
        if let Some(paddr) = H::alloc_frames(root_frames, root_align) {
            let ptr = H::phys_to_virt(paddr).as_mut_ptr();
            unsafe { core::ptr::write_bytes(ptr, 0, PAGE_SIZE_4K * root_frames) };
            Ok(paddr)
        } else {
            Err(PagingError::NoMemory)
        }
    }

    fn table_of<'a>(&self, paddr: PhysAddr) -> &'a [PTE] {
        let ptr = H::phys_to_virt(paddr).as_ptr() as _;
        unsafe { core::slice::from_raw_parts(ptr, ENTRY_COUNT) }
    }

    fn table_of_mut<'a>(&mut self, paddr: PhysAddr) -> &'a mut [PTE] {
        let ptr = H::phys_to_virt(paddr).as_mut_ptr() as _;
        unsafe { core::slice::from_raw_parts_mut(ptr, ENTRY_COUNT) }
    }

    fn next_table<'a>(&self, entry: &PTE) -> PagingResult<&'a [PTE]> {
        // Descend only into a genuine table. Using `is_table()` (not `!is_huge()`)
        // is essential: on aarch64/riscv a *not-present* huge block reads
        // `is_huge() == false`, so the old check walked into its data frame as a
        // page table (mis-reads, and a wrongful free in `dealloc_tree`).
        if entry.is_table() {
            Ok(self.table_of(entry.paddr()))
        } else if entry.paddr().as_usize() == 0 {
            Err(PagingError::NotMapped)
        } else {
            Err(PagingError::MappedToHugePage)
        }
    }

    fn next_table_mut<'a>(&mut self, entry: &PTE) -> PagingResult<&'a mut [PTE]> {
        if entry.is_table() {
            Ok(self.table_of_mut(entry.paddr()))
        } else if entry.paddr().as_usize() == 0 {
            Err(PagingError::NotMapped)
        } else {
            Err(PagingError::MappedToHugePage)
        }
    }

    /// Whether every entry of the table at `table_paddr` is unused (all leaves
    /// unmapped). Used to detect an intermediate table left empty by a prior
    /// huge-page split so a later huge `map` can reclaim it.
    fn table_all_unused(&self, table_paddr: PhysAddr) -> bool {
        self.table_of(table_paddr).iter().all(|e| e.is_unused())
    }

    fn next_table_mut_or_create<'a>(&mut self, entry: &mut PTE) -> PagingResult<&'a mut [PTE]> {
        if entry.is_unused() {
            let paddr = Self::alloc_table()?;
            *entry = GenericPTE::new_table(paddr);
            Ok(self.table_of_mut(paddr))
        } else {
            self.next_table_mut(entry)
        }
    }

    fn get_entry(&self, vaddr: M::VirtAddr) -> PagingResult<(&PTE, PageSize)> {
        let vaddr: usize = vaddr.into();
        let p3 = if M::LEVELS == 3 {
            self.table_of(self.root_paddr())
        } else if M::LEVELS == 4 {
            let p4 = self.table_of(self.root_paddr());
            let p4e = &p4[p4_index(vaddr)];
            self.next_table(p4e)?
        } else {
            unreachable!()
        };
        let p3e = &p3[p3_index(vaddr)];
        if p3e.is_huge() {
            return Ok((p3e, PageSize::Size1G));
        }

        let p2 = self.next_table(p3e)?;
        let p2e = &p2[p2_index(vaddr)];
        if p2e.is_huge() {
            return Ok((p2e, PageSize::Size2M));
        }

        let p1 = self.next_table(p2e)?;
        let p1e = &p1[p1_index(vaddr)];
        Ok((p1e, PageSize::Size4K))
    }

    fn get_entry_mut(&mut self, vaddr: M::VirtAddr) -> PagingResult<(&mut PTE, PageSize)> {
        let vaddr: usize = vaddr.into();
        let p3 = if M::LEVELS == 3 {
            self.table_of_mut(self.root_paddr())
        } else if M::LEVELS == 4 {
            let p4 = self.table_of_mut(self.root_paddr());
            let p4e = &mut p4[p4_index(vaddr)];
            self.next_table_mut(p4e)?
        } else {
            unreachable!()
        };
        let p3e = &mut p3[p3_index(vaddr)];
        if p3e.is_huge() {
            return Ok((p3e, PageSize::Size1G));
        }

        let p2 = self.next_table_mut(p3e)?;
        let p2e = &mut p2[p2_index(vaddr)];
        if p2e.is_huge() {
            return Ok((p2e, PageSize::Size2M));
        }

        let p1 = self.next_table_mut(p2e)?;
        let p1e = &mut p1[p1_index(vaddr)];
        Ok((p1e, PageSize::Size4K))
    }

    fn get_entry_mut_or_create(
        &mut self,
        vaddr: M::VirtAddr,
        page_size: PageSize,
    ) -> PagingResult<&mut PTE> {
        let vaddr: usize = vaddr.into();
        let p3 = if M::LEVELS == 3 {
            self.table_of_mut(self.root_paddr())
        } else if M::LEVELS == 4 {
            let p4 = self.table_of_mut(self.root_paddr());
            let p4e = &mut p4[p4_index(vaddr)];
            self.next_table_mut_or_create(p4e)?
        } else {
            unreachable!()
        };
        let p3e = &mut p3[p3_index(vaddr)];
        if page_size == PageSize::Size1G {
            return Ok(p3e);
        }

        let p2 = self.next_table_mut_or_create(p3e)?;
        let p2e = &mut p2[p2_index(vaddr)];
        if page_size == PageSize::Size2M {
            return Ok(p2e);
        }

        let p1 = self.next_table_mut_or_create(p2e)?;
        let p1e = &mut p1[p1_index(vaddr)];
        Ok(p1e)
    }

    fn walk_recursive<F>(
        &self,
        table: &[PTE],
        level: usize,
        start_vaddr: M::VirtAddr,
        limit: usize,
        pre_func: Option<&F>,
        post_func: Option<&F>,
    ) where
        F: Fn(usize, usize, M::VirtAddr, &PTE),
    {
        let start_vaddr_usize: usize = start_vaddr.into();
        let mut n = 0;
        for (i, entry) in table.iter().enumerate() {
            let vaddr_usize = start_vaddr_usize + (i << (12 + (M::LEVELS - 1 - level) * 9));
            let vaddr = vaddr_usize.into();

            if entry.is_present() {
                if let Some(func) = pre_func {
                    func(level, i, vaddr, entry);
                }
                if level < M::LEVELS - 1
                    && !entry.is_huge()
                    && let Ok(table) = self.next_table(entry)
                {
                    self.walk_recursive(table, level + 1, vaddr, limit, pre_func, post_func);
                }
                if let Some(func) = post_func {
                    func(level, i, vaddr, entry);
                }
                n += 1;
                if n >= limit {
                    break;
                }
            }
        }
    }

    fn dealloc_tree(&self, table_paddr: PhysAddr, level: usize) {
        // don't free the entries in last level, they are not array.
        if level < M::LEVELS - 1 {
            for entry in self.table_of(table_paddr) {
                if self.next_table(entry).is_ok() {
                    self.dealloc_tree(entry.paddr(), level + 1);
                }
            }
        }
        H::dealloc_frame(table_paddr);
    }

    /// Reclaims intermediate tables that an unmap of `[vaddr, vaddr + size)`
    /// leaves entirely empty — e.g. an L2->L3 table stranded when a huge-page
    /// split's leaves are unmapped. Each such table's frame is freed and its
    /// parent entry cleared (break-before-make), instead of the table lingering
    /// until the whole page table is dropped. The root table is never freed.
    /// Safe to call after any range unmap; a no-op when nothing became empty.
    pub fn reclaim_empty_tables(&mut self, vaddr: M::VirtAddr, size: usize) {
        if size == 0 {
            return;
        }
        let lo: usize = vaddr.into();
        let hi = lo.saturating_add(size);
        let root = self.root_paddr();
        // The root table is never freed; ignore its emptiness.
        let _ = self.reclaim_empty_in_range(root, 0, 0, lo, hi);
    }

    /// Recursively frees the empty descendant tables of `table_paddr` (which
    /// covers `[base, ..)` at `level`) that lie within `[lo, hi)`, clearing each
    /// freed child's parent entry. Returns whether `table_paddr` is now fully
    /// unused. Follows the crate's `table_of_mut` fabricated-lifetime convention;
    /// recursion only ever touches disjoint child tables.
    fn reclaim_empty_in_range(
        &mut self,
        table_paddr: PhysAddr,
        level: usize,
        base: usize,
        lo: usize,
        hi: usize,
    ) -> bool {
        // A last-level (leaf) table holds pages, not sub-tables: report only
        // whether it is empty so the parent can decide to free it.
        if level >= M::LEVELS - 1 {
            return self.table_all_unused(table_paddr);
        }
        let entry_span = 1usize << (12 + (M::LEVELS - 1 - level) * 9);
        let table = self.table_of_mut(table_paddr);
        let mut all_children_freed = true;
        for (i, entry) in table.iter_mut().enumerate() {
            let entry_base = base + i * entry_span;
            if entry_base >= hi || entry_base.saturating_add(entry_span) <= lo {
                continue; // this entry's span is entirely outside the unmapped range
            }
            // A root subtree shared from another page table via `copy_from` is not
            // ours to free (mirrors `Drop`).
            #[cfg(feature = "copy-from")]
            if level == 0 && self.borrowed_entries.get(i) {
                all_children_freed = false;
                continue;
            }
            // Descend only into a genuine sub-table. `is_table()` excludes an
            // unused slot, a huge block, and — crucially — a *not-present* huge
            // block (whose `is_huge()` reads false on aarch64/riscv), whose data
            // frame must never be misread as a page table and freed.
            if !entry.is_table() {
                continue;
            }
            let child_paddr = entry.paddr();
            if self.reclaim_empty_in_range(child_paddr, level + 1, entry_base, lo, hi) {
                // Break-before-make: clear the parent entry and complete the TLBI
                // (invalidating the cached walk of this table) before freeing the
                // now-unreferenced table frame.
                entry.clear();
                M::flush_tlb(Some(entry_base.into()));
                H::dealloc_frame(child_paddr);
            } else {
                all_children_freed = false;
            }
        }
        // Only a table whose every in-range sub-table was freed can have become
        // empty; scan to confirm no out-of-range entries survive.
        all_children_freed && self.table_all_unused(table_paddr)
    }
}

impl<M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> Drop for PageTable64<M, PTE, H> {
    fn drop(&mut self) {
        let root = self.table_of(self.root_paddr);
        #[allow(unused_variables)]
        for (i, entry) in root.iter().enumerate() {
            #[cfg(feature = "copy-from")]
            if self.borrowed_entries.get(i) {
                continue;
            }
            if self.next_table(entry).is_ok() {
                self.dealloc_tree(entry.paddr(), 1);
            }
        }
        if self.root_allocated_with_frames {
            H::dealloc_frames(self.root_paddr(), self.root_frames);
        } else {
            H::dealloc_frame(self.root_paddr());
        }
    }
}

/// A cursor created by [`PageTable64::cursor`] to modify the page table.
pub struct PageTable64Cursor<'a, M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> {
    inner: &'a mut PageTable64<M, PTE, H>,
    flusher: TlbFlusher<M>,
}

impl<M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> Deref
    for PageTable64Cursor<'_, M, PTE, H>
{
    type Target = PageTable64<M, PTE, H>;

    fn deref(&self) -> &PageTable64<M, PTE, H> {
        self.inner
    }
}

impl<'a, M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> PageTable64Cursor<'a, M, PTE, H> {
    fn new(inner: &'a mut PageTable64<M, PTE, H>) -> Self {
        Self {
            inner,
            flusher: TlbFlusher::None,
        }
    }

    fn push(&mut self, vaddr: M::VirtAddr) {
        match self.flusher {
            TlbFlusher::None => {
                let mut arr = ArrayVec::new();
                arr.push(vaddr);
                self.flusher = TlbFlusher::Array(arr);
            }
            TlbFlusher::Array(ref mut arr) => {
                if arr.try_push(vaddr).is_err() {
                    self.flusher = TlbFlusher::Full;
                }
            }
            TlbFlusher::Full => {}
        }
    }

    /// Maps a virtual page to a physical frame with the given `page_size`
    /// and mapping `flags`.
    ///
    /// The virtual page starts at `vaddr`, and the physical frame starts at
    /// `target`. If the `target` is not aligned to the `page_size`, it will be
    /// aligned down automatically.
    ///
    /// Returns [`Err(PagingError::AlreadyMapped)`](PagingError::AlreadyMapped)
    /// if the mapping is already present.
    pub fn map(
        &mut self,
        vaddr: M::VirtAddr,
        target: PhysAddr,
        page_size: PageSize,
        flags: MappingFlags,
    ) -> PagingResult {
        // `vaddr` does not need to be page-aligned here; `get_entry_mut_or_create`
        // internally maps `vaddr` to its corresponding page table entry (PTE).
        {
            let entry = self.inner.get_entry_mut_or_create(vaddr, page_size)?;
            if !entry.is_unused() {
                // Occupied. Installing a huge page over a leftover *intermediate
                // table* (a prior split whose leaves are now all unmapped) is the
                // one recoverable case: reclaim the empty table below. `is_table()`
                // excludes a live mapping, a (present or not-present) huge block —
                // whose data frame must not be misread as a table — and any other
                // non-table entry; a table that still holds finer mappings is
                // rejected by `table_all_unused` further down.
                if !(page_size.is_huge() && entry.is_table()) {
                    return Err(PagingError::AlreadyMapped);
                }
            } else {
                *entry =
                    GenericPTE::new_page(target.align_down(page_size), flags, page_size.is_huge());
                // Fresh not-present → present (the `is_unused` check guarantees it).
                // Architectures that never cache not-present translations need no
                // TLB maintenance here; skipping it removes a broadcast TLBI per
                // faulted page. See `PagingMetaData::NEED_FLUSH_ON_MAP`.
                // `remap`/`protect`/`unmap` touch *valid* entries and still flush.
                if M::NEED_FLUSH_ON_MAP {
                    self.push(vaddr);
                }
                return Ok(());
            }
        }

        // Huge map over a leftover intermediate table. Reclaim it iff it is empty
        // (an emptied split table); otherwise it holds live finer mappings and the
        // huge block cannot replace them.
        //
        // Assumes the intermediate table is exclusively owned by this page table
        // (true for a table produced by a huge-page split, and for private user
        // mappings). `copy_from` shares subtrees only at the root level and only
        // fully-populated ones, which are never `table_all_unused`, so a borrowed
        // subtree is never freed here.
        let table_paddr = self.inner.get_entry_mut_or_create(vaddr, page_size)?.paddr();
        if !self.inner.table_all_unused(table_paddr) {
            return Err(PagingError::AlreadyMapped);
        }
        // Break-before-make: clear the table descriptor and complete the TLBI
        // (invalidating any cached walk of this table) before freeing the table
        // frame, then install the huge block into the now-unused slot.
        self.inner.get_entry_mut_or_create(vaddr, page_size)?.clear();
        M::flush_tlb(Some(vaddr));
        H::dealloc_frame(table_paddr);

        let entry = self.inner.get_entry_mut_or_create(vaddr, page_size)?;
        debug_assert!(entry.is_unused(), "reclaimed slot must be free");
        *entry = GenericPTE::new_page(target.align_down(page_size), flags, page_size.is_huge());
        if M::NEED_FLUSH_ON_MAP {
            self.push(vaddr);
        }
        Ok(())
    }

    /// Remaps the mapping starting at `vaddr`, updates both the physical
    /// address and flags.
    ///
    /// Returns the page size of the mapping.
    ///
    /// Returns [`Err(PagingError::NotMapped)`](PagingError::NotMapped) if the
    /// intermediate level tables of the mapping is not present.
    pub fn remap(
        &mut self,
        vaddr: M::VirtAddr,
        paddr: PhysAddr,
        flags: MappingFlags,
    ) -> PagingResult<PageSize> {
        let (entry, size) = self.inner.get_entry_mut(vaddr)?;
        entry.set_paddr(paddr);
        entry.set_flags(flags, size.is_huge());
        self.push(vaddr);
        Ok(size)
    }

    /// Updates the flags of the mapping starting at `vaddr`.
    ///
    /// Returns the page size of the mapping.
    ///
    /// Returns [`Err(PagingError::NotMapped)`](PagingError::NotMapped) if the
    /// mapping is not present.
    pub fn protect(&mut self, vaddr: M::VirtAddr, flags: MappingFlags) -> PagingResult<PageSize> {
        let (entry, size) = self.inner.get_entry_mut(vaddr)?;
        if !entry.is_present() {
            return Err(PagingError::NotMapped);
        }
        entry.set_flags(flags, size.is_huge());
        self.push(vaddr);
        Ok(size)
    }

    /// Unmaps the mapping starting at `vaddr`.
    ///
    /// Returns [`Err(PagingError::NotMapped)`](PagingError::NotMapped) if the
    /// mapping is not present.
    pub fn unmap(
        &mut self,
        vaddr: M::VirtAddr,
    ) -> PagingResult<(PhysAddr, MappingFlags, PageSize)> {
        let (entry, size) = self.inner.get_entry_mut(vaddr)?;
        if !entry.is_present() {
            entry.clear();
            return Err(PagingError::NotMapped);
        }
        let paddr = entry.paddr();
        let flags = entry.flags();
        entry.clear();
        self.push(vaddr);
        Ok((paddr, flags, size))
    }

    /// Splits the huge-page mapping at `vaddr` by pointing its block descriptor
    /// at the caller-provided, pre-zeroed intermediate table `table_paddr`
    /// (obtained from [`PageTable64::alloc_intermediate_table`]).
    ///
    /// The region is then mapped by the (empty) next-level table; the caller
    /// installs the finer leaf entries with [`map`](Self::map), which cannot
    /// allocate because the table already exists. Because *this* call performs no
    /// allocation, a split whose table was reserved ahead of time commits without
    /// ever failing for lack of memory partway through and leaving a half-torn
    /// mapping (the failure mode of unmapping the block and then allocating the
    /// leaf table on the first re-`map`).
    ///
    /// Break-before-make: replacing a valid *block* descriptor with a valid
    /// *table* descriptor of a finer granule for the same VA is architecturally
    /// unsafe (a sibling core may cache both translations and take a TLB conflict
    /// abort). This invalidates the block and completes the broadcast TLBI
    /// *before* installing the table, so only one translation for the VA is ever
    /// live. Installing the table over the now-invalid slot is a not-present ->
    /// present transition needing no further flush; the caller maps the leaves
    /// (also not-present -> present) afterwards. The region is transiently
    /// unmapped across these steps — callers hold the address-space lock, so a
    /// concurrent fault blocks and re-resolves rather than seeing a conflict.
    ///
    /// Returns [`Err(PagingError::NotMapped)`](PagingError::NotMapped) if `vaddr`
    /// is not mapped by a present huge page, leaving the mapping unchanged and
    /// `table_paddr` for the caller to free.
    pub fn split_huge_page_with(
        &mut self,
        vaddr: M::VirtAddr,
        table_paddr: PhysAddr,
    ) -> PagingResult {
        let (entry, size) = self.inner.get_entry_mut(vaddr)?;
        if !size.is_huge() || !entry.is_present() {
            return Err(PagingError::NotMapped);
        }
        // Break: invalidate the block and complete the broadcast TLBI
        // (`flush_tlb` ends with `dsb sy; isb`) so no core still caches the huge
        // entry. Make: install the pre-reserved table over the invalid slot.
        entry.clear();
        M::flush_tlb(Some(vaddr));
        *entry = GenericPTE::new_table(table_paddr);
        Ok(())
    }

    /// Maps a contiguous virtual memory region to a contiguous physical memory
    /// region with the given mapping `flags`.
    ///
    /// The virtual and physical memory regions start at `vaddr` and `paddr`
    /// respectively. The region size is `size`. The addresses and `size` must
    /// be aligned to 4K, otherwise it will return
    /// [`Err(PagingError::NotAligned)`].
    ///
    /// When `allow_huge` is true, it will try to map the region with huge pages
    /// if possible. Otherwise, it will map the region with 4K pages.
    ///
    /// [`Err(PagingError::NotAligned)`]: PagingError::NotAligned
    pub fn map_region(
        &mut self,
        vaddr: M::VirtAddr,
        get_paddr: impl Fn(M::VirtAddr) -> PhysAddr,
        size: usize,
        flags: MappingFlags,
        allow_huge: bool,
    ) -> PagingResult {
        let mut vaddr_usize: usize = vaddr.into();
        let mut size = size;
        if !PageSize::Size4K.is_aligned(vaddr_usize) || !PageSize::Size4K.is_aligned(size) {
            return Err(PagingError::NotAligned);
        }
        trace!(
            "map_region({:#x}): [{:#x}, {:#x}) {:?}",
            self.root_paddr(),
            vaddr_usize,
            vaddr_usize + size,
            flags,
        );
        while size > 0 {
            let vaddr = vaddr_usize.into();
            let paddr = get_paddr(vaddr);
            let page_size = if allow_huge {
                if PageSize::Size1G.is_aligned(vaddr_usize)
                    && paddr.is_aligned(PageSize::Size1G)
                    && size >= PageSize::Size1G as usize
                {
                    PageSize::Size1G
                } else if PageSize::Size2M.is_aligned(vaddr_usize)
                    && paddr.is_aligned(PageSize::Size2M)
                    && size >= PageSize::Size2M as usize
                {
                    PageSize::Size2M
                } else {
                    PageSize::Size4K
                }
            } else {
                PageSize::Size4K
            };
            self.map(vaddr, paddr, page_size, flags).inspect_err(|e| {
                error!("failed to map page: {vaddr_usize:#x?}({page_size:?}) -> {paddr:#x?}, {e:?}")
            })?;

            vaddr_usize += page_size as usize;
            size -= page_size as usize;
        }
        Ok(())
    }

    /// Unmaps a contiguous virtual memory region.
    ///
    /// The region must be mapped before using [`Self::map_region`], or
    /// unexpected behaviors may occur. It can deal with huge pages
    /// automatically.
    pub fn unmap_region(&mut self, vaddr: M::VirtAddr, size: usize) -> PagingResult {
        let mut vaddr_usize: usize = vaddr.into();
        let mut size = size;
        trace!(
            "unmap_region({:#x}) [{:#x}, {:#x})",
            self.root_paddr(),
            vaddr_usize,
            vaddr_usize + size,
        );
        while size > 0 {
            let vaddr = vaddr_usize.into();
            let (_, _, page_size) = self
                .unmap(vaddr)
                .inspect_err(|e| error!("failed to unmap page: {vaddr_usize:#x?}, {e:?}"))?;

            assert!(page_size.is_aligned(vaddr_usize));
            assert!(page_size as usize <= size);
            vaddr_usize += page_size as usize;
            size -= page_size as usize;
        }
        Ok(())
    }

    /// Updates mapping flags of a contiguous virtual memory region.
    ///
    /// The region must be mapped before using [`Self::map_region`], or
    /// unexpected behaviors may occur. It can deal with huge pages
    /// automatically.
    pub fn protect_region(
        &mut self,
        vaddr: M::VirtAddr,
        size: usize,
        flags: MappingFlags,
    ) -> PagingResult {
        let mut vaddr_usize: usize = vaddr.into();
        let mut size = size;
        trace!(
            "protect_region({:#x}) [{:#x}, {:#x}) {:?}",
            self.root_paddr(),
            vaddr_usize,
            vaddr_usize + size,
            flags,
        );
        while size > 0 {
            let vaddr = vaddr_usize.into();
            let page_size = match self.inner.get_entry_mut(vaddr) {
                Ok((entry, page_size)) => {
                    if !entry.is_unused() {
                        entry.set_flags(flags, page_size.is_huge());
                        self.push(vaddr);
                        page_size
                    } else {
                        PageSize::Size4K
                    } // ignore if unused
                }
                Err(PagingError::NotMapped) => PageSize::Size4K,
                Err(e) => {
                    error!("failed to protect page: {vaddr_usize:#x?}, {e:?}");
                    return Err(e);
                }
            };

            assert!(page_size.is_aligned(vaddr_usize));
            assert!(page_size as usize <= size);
            vaddr_usize += page_size as usize;
            size -= page_size as usize;
        }
        Ok(())
    }

    /// Copy entries from another page table within the given virtual memory
    /// range.
    #[cfg(feature = "copy-from")]
    pub fn copy_from(&mut self, other: &PageTable64<M, PTE, H>, start: M::VirtAddr, size: usize) {
        if size == 0 {
            return;
        }
        let src_table = self.table_of(other.root_paddr);
        let root_paddr = self.root_paddr;
        let dst_table = self.inner.table_of_mut(root_paddr);
        let index_fn = if M::LEVELS == 3 {
            p3_index
        } else if M::LEVELS == 4 {
            p4_index
        } else {
            unreachable!()
        };
        let start_idx = index_fn(start.into());
        let end_idx = index_fn(start.into() + size - 1) + 1;
        assert!(start_idx < ENTRY_COUNT);
        assert!(end_idx <= ENTRY_COUNT);
        for i in start_idx..end_idx {
            let entry = &mut dst_table[i];
            if !self.inner.borrowed_entries.set(i, true) && self.next_table(entry).is_ok() {
                self.dealloc_tree(entry.paddr(), 1);
            }
            *entry = src_table[i];
        }
        self.flusher = TlbFlusher::Full;
    }

    /// Flushes the TLB according to the recorded flush requests.
    pub fn flush(&mut self) {
        #[cfg(not(docsrs))]
        match &self.flusher {
            TlbFlusher::None => {}
            TlbFlusher::Array(addrs) => {
                for vaddr in addrs.iter() {
                    M::flush_tlb(Some(*vaddr));
                }
            }
            TlbFlusher::Full => {
                M::flush_tlb(None);
            }
        }
        self.flusher = TlbFlusher::None;
    }
}

impl<M: PagingMetaData, PTE: GenericPTE, H: PagingHandler> Drop
    for PageTable64Cursor<'_, M, PTE, H>
{
    fn drop(&mut self) {
        self.flush();
    }
}
