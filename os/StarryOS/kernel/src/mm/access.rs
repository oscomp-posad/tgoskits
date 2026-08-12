use alloc::string::String;
use core::{
    alloc::Layout,
    ffi::c_char,
    hint::{spin_loop, unlikely},
    mem::{MaybeUninit, transmute},
    ptr, slice,
    sync::atomic::{AtomicU32, AtomicU64, Ordering},
};

use ax_errno::{AxError, AxResult};
use ax_io::prelude::*;
use ax_memory_addr::{MemoryAddr, VirtAddr};
#[cfg(feature = "user-access-fastpath")]
use ax_memory_addr::PAGE_SIZE_4K;
use ax_runtime::hal::{
    cpu::{asm::user_copy, trap::page_fault_handler},
    paging::MappingFlags,
};
use ax_task::{current, might_sleep};
use extern_trait::extern_trait;
use starry_vm::{VmError, VmIo, VmResult, vm_load_until_nul, vm_read_slice, vm_write_slice};

use crate::{
    config::{USER_SPACE_BASE, USER_SPACE_SIZE},
    task::AsThread,
};

/// Enables scoped access into user memory, allowing page faults to occur inside
/// kernel.
#[track_caller]
pub fn access_user_memory<R>(f: impl FnOnce() -> R) -> R {
    assert!(
        ax_runtime::hal::cpu::asm::irqs_enabled(),
        "faultable user memory access requires IRQs enabled"
    );
    might_sleep();

    let curr = current();
    let Some(thr) = curr.try_as_thread() else {
        panic!("access_user_memory called outside of thread context");
    };

    thr.set_accessing_user_memory(true);
    let result = f();
    thr.set_accessing_user_memory(false);
    result
}

/// Maximum range (in 4 KiB pages) eligible for the lock-free fast path. Bounds
/// the IRQs-off window of the page-by-page hardware probe. Pipe IPC messages and
/// syscall-argument structs are far smaller than this; larger transfers take the
/// slow path, where the aspace lock is amortized over a large copy anyway.
#[cfg(feature = "user-access-fastpath")]
const FASTPATH_MAX_PAGES: usize = 16;

/// Lock-free check that every page of `[start, start+len)` is already present
/// with the requested EL0 permission, using a lock-free hardware page-permission
/// probe (`AT S1E0R`/`S1E0W`, which resolves the address against the current
/// stage-1 tables with EL0 access rights) and no address-space lock.
///
/// Returns `true` only when the whole range is fast-path eligible and every page
/// is present+permitted, in which case the caller may skip the aspace
/// lock+`populate_area` entirely: a present, EL0-permitted page is by
/// construction one the user could access itself, so the copy/dereference is
/// legitimate and will not fault. Returns `false` — deferring to the unchanged
/// locked slow path — when the range is empty or too large, when any page is not
/// present or lacks the permission (cold, unmapped, or copy-on-write read-only),
/// or on architectures without a probe.
#[cfg(feature = "user-access-fastpath")]
fn user_range_fast_ok(start: VirtAddr, len: usize, access_flags: MappingFlags) -> bool {
    if len == 0 {
        return false;
    }
    // Checked arithmetic, mirroring the slow path's `VirtAddrRange::try_from_start_size`
    // + page rounding: reject to the slow path on any address-space overflow rather
    // than relying on wrap semantics. `check_region` reaches this with a fully
    // caller-controlled `start` and no prior range bound, so a hostile top-of-space
    // pointer must not overflow here (which would panic under an overflow-checks
    // build); it simply falls through to `can_access_range`, which rejects it.
    let start = start.as_usize();
    let Some(end) = start.checked_add(len) else {
        return false;
    };
    let page_start = start & !(PAGE_SIZE_4K - 1);
    let Some(page_end) = end
        .checked_add(PAGE_SIZE_4K - 1)
        .map(|v| v & !(PAGE_SIZE_4K - 1))
    else {
        return false;
    };
    // `end >= start` and both are rounded the same way, so `page_end >= page_start`.
    let pages = (page_end - page_start) / PAGE_SIZE_4K;
    // `pages == 0` is unreachable here: the `len == 0` early return plus
    // `page_end > page_start` guarantee `pages >= 1`. It is kept as a defensive
    // guard so the range cap still holds if either invariant is later removed.
    if pages == 0 || pages > FASTPATH_MAX_PAGES {
        return false;
    }
    // A write access requires the page to be present *and* EL0-writable; a
    // copy-on-write page is present-read-only, so a write probe correctly misses
    // and routes to the slow path where `populate_area` performs the COW copy.
    let write = access_flags.contains(MappingFlags::WRITE);

    // IRQs off across the whole probe: `PAR_EL1` is a per-CPU scratch register
    // shared with any interrupt handler that also executes an `AT`. Disabling
    // IRQs guarantees no other `AT` runs on this CPU between our `AT` and the
    // `mrs` that reads the result. The range is capped, so the window is a
    // handful of instructions.
    let _guard = ax_kernel_guard::NoPreemptIrqSave::new();
    let mut page = page_start;
    while page < page_end {
        if !ax_runtime::hal::cpu::asm::user_access_ok_page(page, write) {
            return false;
        }
        page += PAGE_SIZE_4K;
    }
    true
}

fn check_region(start: VirtAddr, layout: Layout, access_flags: MappingFlags) -> AxResult<()> {
    let align = layout.align();
    if start.as_usize() & (align - 1) != 0 {
        return Err(AxError::BadAddress);
    }

    let curr = current();
    let Some(thr) = curr.try_as_thread() else {
        warn!(
            "reject user region check outside thread context: task={}, start={:#x}, len={}",
            curr.id_name(),
            start.as_usize(),
            layout.size()
        );
        return Err(AxError::BadAddress);
    };
    let aspace_arc = thr.proc_data.aspace();
    if unsafe { aspace_arc.raw() }.is_owned_by_current() {
        return Err(AxError::BadAddress);
    }

    // Lock-free fast path: if every page is already present with the requested
    // permission, the later dereference will not fault, so skip the aspace lock
    // and `populate_area`. Misses fall through to the locked slow path.
    #[cfg(feature = "user-access-fastpath")]
    if user_range_fast_ok(start, layout.size(), access_flags) {
        return Ok(());
    }

    let mut aspace = aspace_arc.lock();

    if !aspace.can_access_range(start, layout.size(), access_flags) {
        return Err(AxError::BadAddress);
    }

    let page_start = start.align_down_4k();
    let page_end = (start + layout.size()).align_up_4k();
    aspace.populate_area(page_start, page_end - page_start, access_flags)?;

    Ok(())
}

/// A pointer to user space memory.
#[repr(transparent)]
#[derive(PartialEq, Clone, Copy)]
pub struct UserPtr<T>(*mut T);

impl<T> From<usize> for UserPtr<T> {
    fn from(value: usize) -> Self {
        UserPtr(value as *mut _)
    }
}

impl<T> From<*mut T> for UserPtr<T> {
    fn from(value: *mut T) -> Self {
        UserPtr(value)
    }
}

impl<T> Default for UserPtr<T> {
    fn default() -> Self {
        Self(ptr::null_mut())
    }
}

impl<T> UserPtr<T> {
    const ACCESS_FLAGS: MappingFlags = MappingFlags::READ.union(MappingFlags::WRITE);

    pub fn address(&self) -> VirtAddr {
        VirtAddr::from_ptr_of(self.0)
    }

    pub fn as_ptr(&self) -> *mut T {
        self.0
    }

    pub fn cast<U>(self) -> UserPtr<U> {
        UserPtr(self.0 as *mut U)
    }

    pub fn is_null(&self) -> bool {
        self.0.is_null()
    }

    pub fn get_as_mut(self) -> AxResult<&'static mut T> {
        check_region(self.address(), Layout::new::<T>(), Self::ACCESS_FLAGS)?;
        Ok(unsafe { &mut *self.0 })
    }

    pub fn get_as_mut_slice(self, len: usize) -> AxResult<&'static mut [T]> {
        if len == 0 {
            return Ok(&mut []);
        }
        check_region(
            self.address(),
            Layout::array::<T>(len).unwrap(),
            Self::ACCESS_FLAGS,
        )?;
        Ok(unsafe { slice::from_raw_parts_mut(self.0, len) })
    }
}

/// Atomically read a naturally-aligned `u32` from user memory as a single load,
/// so a concurrent userspace atomic store cannot be observed torn.
///
/// `vm_read::<u32>()` goes through the byte-wise `user_copy` memcpy (four `ldrb` on
/// aarch64), which is NOT single-copy-atomic — under SMP a racing userspace store to
/// the word can interleave the byte reads and yield a value that never existed. The
/// futex value-compare (FUTEX_WAIT and the race-closing re-check) must read the word
/// atomically, exactly like Linux's `get_user`, or it can spuriously match/mismatch
/// and — at the re-check — block through a concurrent wake (lost wakeup). This mirrors
/// [`atomic_update_user_u32`]: `check_region` validates a present, EL0-readable,
/// aligned word, then the access happens through an `AtomicU32` inside the
/// user-memory-access window, lowering to one atomic load on every architecture.
pub fn atomic_read_user_u32(ptr: *const u32) -> AxResult<u32> {
    check_region(
        VirtAddr::from_ptr_of(ptr),
        Layout::new::<u32>(),
        MappingFlags::READ,
    )?;

    let ptr = ptr.cast::<AtomicU32>();
    Ok(access_user_memory(|| {
        // SAFETY: check_region() validated that the user address is a readable,
        // properly aligned u32 in the current address space.
        unsafe { &*ptr }.load(Ordering::Acquire)
    }))
}

pub fn atomic_update_user_u32(
    ptr: *mut u32,
    mut update: impl FnMut(u32) -> AxResult<u32>,
) -> AxResult<u32> {
    check_region(
        VirtAddr::from_ptr_of(ptr),
        Layout::new::<u32>(),
        MappingFlags::READ.union(MappingFlags::WRITE),
    )?;

    let ptr = ptr.cast::<AtomicU32>();
    access_user_memory(|| {
        loop {
            // SAFETY: check_region() validated that the user address is a
            // writable, properly aligned u32 in the current address space.
            let old = unsafe { &*ptr }.load(Ordering::SeqCst);
            let new = update(old)?;
            match unsafe { &*ptr }.compare_exchange_weak(
                old,
                new,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(_) => return Ok(old),
                Err(_) => spin_loop(),
            }
        }
    })
}

/// An immutable pointer to user space memory.
#[repr(transparent)]
#[derive(PartialEq, Clone, Copy)]
pub struct UserConstPtr<T>(*const T);

impl<T> From<usize> for UserConstPtr<T> {
    fn from(value: usize) -> Self {
        UserConstPtr(value as *const _)
    }
}

impl<T> From<*const T> for UserConstPtr<T> {
    fn from(value: *const T) -> Self {
        UserConstPtr(value)
    }
}

impl<T> Default for UserConstPtr<T> {
    fn default() -> Self {
        Self(ptr::null())
    }
}

impl<T> UserConstPtr<T> {
    const ACCESS_FLAGS: MappingFlags = MappingFlags::READ;

    pub fn address(&self) -> VirtAddr {
        VirtAddr::from_ptr_of(self.0)
    }

    pub fn cast<U>(self) -> UserConstPtr<U> {
        UserConstPtr(self.0 as *const U)
    }

    pub fn is_null(&self) -> bool {
        self.0.is_null()
    }

    pub fn get_as_ref(self) -> AxResult<&'static T> {
        check_region(self.address(), Layout::new::<T>(), Self::ACCESS_FLAGS)?;
        Ok(unsafe { &*self.0 })
    }

    pub fn get_as_slice(self, len: usize) -> AxResult<&'static [T]> {
        if len == 0 {
            return Ok(&[]);
        }
        check_region(
            self.address(),
            Layout::array::<T>(len).unwrap(),
            Self::ACCESS_FLAGS,
        )?;
        Ok(unsafe { slice::from_raw_parts(self.0, len) })
    }
}

macro_rules! nullable {
    ($ptr:ident.$func:ident($($arg:expr),*)) => {
        if $ptr.is_null() {
            Ok(None)
        } else {
            Some($ptr.$func($($arg),*)).transpose()
        }
    };
}

pub(crate) use nullable;

/// Cumulative count of user page faults dispatched to the demand-paging handler.
///
/// Every fault that reaches the address-space `handle_page_fault` call is counted, matching the
/// Linux `pgfault` event in mm/vmstat.c (all minor + major faults, regardless of resolution).
/// Exposed through `/proc/vmstat` so node_exporter's vmstat collector can surface
/// `node_vmstat_pgfault`.
pub static PAGE_FAULT_COUNT: AtomicU64 = AtomicU64::new(0);

#[page_fault_handler]
fn handle_page_fault(vaddr: VirtAddr, access_flags: MappingFlags) -> bool {
    debug!("Page fault at {vaddr:#x}, access_flags: {access_flags:#x?}");

    #[cfg(feature = "stack-guard-page")]
    if ax_task::diagnose_current_stack_guard_page_fault(vaddr) {
        return false;
    }

    let curr = current();
    let Some(thr) = curr.try_as_thread() else {
        return false;
    };

    // Count this fault for any `PERF_COUNT_SW_PAGE_FAULTS` event on the thread
    // (cheap no-op when none exists).
    crate::perf::sw::on_page_fault(thr);

    if unlikely(!thr.is_accessing_user_memory()) {
        // Still try to handle kernel-mode faults on user-space addresses.
        // Several syscall sites (e.g. event.rs, net/io.rs, fs/lock.rs) obtain
        // a direct `&mut` reference into user memory via get_as_mut /
        // get_as_mut_slice and write through it outside of
        // access_user_memory().  If a concurrent fork has re-marked the page
        // read-only between check_region() and the write, the kernel write
        // hits a COW #PF with no fixup-table entry and panics.  Handling the
        // fault here lets the standard COW path copy the page just as it
        // would for a user-mode write.
        let user_range = USER_SPACE_BASE..USER_SPACE_BASE + USER_SPACE_SIZE;
        if !user_range.contains(&vaddr.as_usize()) {
            return false;
        }
        // Avoid recursion / deadlock: if this thread already holds the
        // aspace lock (e.g. fault inside aspace.lock().handle_page_fault())
        // we have to bail out instead of trying to lock it again.
        let aspace_arc = thr.proc_data.aspace();
        if unsafe { aspace_arc.raw() }.is_owned_by_current() {
            return false;
        }
    }

    might_sleep();
    let aspace_arc = thr.proc_data.aspace();
    if unsafe { aspace_arc.raw() }.is_owned_by_current() {
        warn!(
            "user page fault while current thread already owns its address-space lock: \
             vaddr={vaddr:#x}, access_flags={access_flags:#x?}"
        );
        return false;
    }
    PAGE_FAULT_COUNT.fetch_add(1, Ordering::Relaxed);
    aspace_arc.lock().handle_page_fault(vaddr, access_flags)
}

pub const PATH_MAX: usize = 4096;

pub fn vm_load_string(ptr: *const c_char) -> AxResult<String> {
    #[allow(clippy::unnecessary_cast)]
    let bytes = vm_load_until_nul(ptr as *const u8)?;
    String::from_utf8(bytes).map_err(|_| AxError::IllegalBytes)
}

pub fn vm_load_path_string(ptr: *const c_char) -> AxResult<String> {
    let path = vm_load_string(ptr)?;
    if path.len() >= PATH_MAX {
        return Err(AxError::NameTooLong);
    }
    Ok(path)
}

struct Vm;

/// Briefly checks if the given memory region is valid user memory.
pub fn check_access(start: usize, len: usize) -> VmResult {
    const USER_SPACE_END: usize = USER_SPACE_BASE + USER_SPACE_SIZE;
    let ok = (USER_SPACE_BASE..USER_SPACE_END).contains(&start) && (USER_SPACE_END - start) >= len;
    if unlikely(!ok) {
        Err(VmError::AccessDenied)
    } else {
        Ok(())
    }
}

fn prepare_user_memory(op: &str, start: usize, len: usize, access_flags: MappingFlags) -> VmResult {
    check_access(start, len)?;
    if len == 0 {
        return Ok(());
    }
    let curr = current();
    let Some(thr) = curr.try_as_thread() else {
        warn!(
            "reject user memory {op} outside thread context: task={}, start={start:#x}, len={len}",
            curr.id_name()
        );
        return Err(VmError::AccessDenied);
    };
    let aspace_arc = thr.proc_data.aspace();
    if unsafe { aspace_arc.raw() }.is_owned_by_current() {
        return Err(VmError::AccessDenied);
    }

    let start = VirtAddr::from(start);

    // Lock-free fast path: if every page is already present with the requested
    // permission, the copy will not fault, so skip the aspace lock and
    // `populate_area`. Misses fall through to the locked slow path (which also
    // preserves the ENOMEM-vs-EFAULT distinction on a genuine out-of-frames).
    #[cfg(feature = "user-access-fastpath")]
    if user_range_fast_ok(start, len, access_flags) {
        return Ok(());
    }

    // Slow path only: compute the page-aligned bounds `populate_area` needs.
    let end = start + len;
    let page_start = start.align_down_4k();
    let page_end = end.align_up_4k();

    let mut aspace = aspace_arc.lock();
    if !aspace.can_access_range(start, len, access_flags) {
        return Err(VmError::AccessDenied);
    }

    // Preserve the real fault-in error instead of collapsing everything to
    // AccessDenied (which maps to EFAULT). In particular a genuine out-of-frames
    // must surface as ENOMEM, not a misleading "Bad address" on a valid pointer.
    // The `check_region` (UserPtr) path already propagates this via `?`; keep the
    // vm_read/vm_write path consistent.
    aspace
        .populate_area(page_start, page_end - page_start, access_flags)
        .map_err(|e| match e {
            AxError::NoMemory => VmError::NoMemory,
            _ => VmError::AccessDenied,
        })
}

#[extern_trait]
unsafe impl VmIo for Vm {
    fn new() -> Self {
        Self
    }

    fn read(&mut self, start: usize, buf: &mut [MaybeUninit<u8>]) -> VmResult {
        if buf.is_empty() {
            return Ok(());
        }
        prepare_user_memory("read", start, buf.len(), MappingFlags::READ)?;
        let failed_at = access_user_memory(|| unsafe {
            user_copy(buf.as_mut_ptr() as *mut _, start as _, buf.len())
        });
        if unlikely(failed_at != 0) {
            Err(VmError::AccessDenied)
        } else {
            Ok(())
        }
    }

    fn write(&mut self, start: usize, buf: &[u8]) -> VmResult {
        if buf.is_empty() {
            return Ok(());
        }
        prepare_user_memory("write", start, buf.len(), MappingFlags::WRITE)?;
        let failed_at = access_user_memory(|| unsafe {
            user_copy(start as _, buf.as_ptr() as *const _, buf.len())
        });
        if unlikely(failed_at != 0) {
            Err(VmError::AccessDenied)
        } else {
            Ok(())
        }
    }
}

/// A read-only buffer in the VM's memory.
///
/// It implements the `ax_io::Read` trait, allowing it to be used with other I/O
/// operations.
pub struct VmBytes {
    /// The pointer to the start of the buffer in the VM's memory.
    pub ptr: *const u8,
    /// The length of the buffer.
    pub len: usize,
}

impl VmBytes {
    /// Creates a new `VmBytes` from a raw pointer and a length.
    pub fn new(ptr: *const u8, len: usize) -> Self {
        Self { ptr, len }
    }
}

impl Read for VmBytes {
    /// Reads bytes from the VM's memory into the provided buffer.
    fn read(&mut self, buf: &mut [u8]) -> ax_io::Result<usize> {
        let len = self.len.min(buf.len());
        vm_read_slice(self.ptr, unsafe {
            transmute::<&mut [u8], &mut [MaybeUninit<u8>]>(&mut buf[..len])
        })?;
        self.ptr = self.ptr.wrapping_add(len);
        self.len -= len;
        Ok(len)
    }
}

impl IoBuf for VmBytes {
    fn remaining(&self) -> usize {
        self.len
    }
}

/// A mutable buffer in the VM's memory.
///
/// It implements the `ax_io::Write` trait, allowing it to be used with other I/O
/// operations.
pub struct VmBytesMut {
    /// The pointer to the start of the buffer in the VM's memory.
    pub ptr: *mut u8,
    /// The length of the buffer.
    pub len: usize,
}

impl VmBytesMut {
    /// Creates a new `VmBytesMut` from a raw pointer and a length.
    pub fn new(ptr: *mut u8, len: usize) -> Self {
        Self { ptr, len }
    }
}

impl Write for VmBytesMut {
    /// Writes bytes from the provided buffer into the VM's memory.
    fn write(&mut self, buf: &[u8]) -> ax_io::Result<usize> {
        let len = self.len.min(buf.len());
        vm_write_slice(self.ptr, &buf[..len])?;
        self.ptr = self.ptr.wrapping_add(len);
        self.len -= len;
        Ok(len)
    }

    /// Flushes the buffer. This is a no-op for `VmBytesMut`.
    fn flush(&mut self) -> ax_io::Result {
        Ok(())
    }
}

impl IoBufMut for VmBytesMut {
    fn remaining_mut(&self) -> usize {
        self.len
    }
}

/// Patches kernel text, ensuring page permissions and instruction-cache
/// synchronization are handled consistently.
pub fn patch_kernel_text<F>(addr: VirtAddr, len: usize, action: F) -> AxResult<()>
where
    F: FnOnce(*mut u8),
{
    if len == 0 {
        return Ok(());
    }

    let aligned_addr = addr.align_down_4k();
    let aligned_length = (addr + len).align_up_4k() - aligned_addr;

    // The kernel address-space lock (`SpinNoIrq`) MUST be acquired *inside* the
    // `stop_machine` critical section, not before it. `stop_machine` itself
    // takes a `SpinNoIrq` (`STOP_MACHINE_LOCK`); acquiring `kernel_aspace`
    // first and then dropping it inside the closure produces a non-LIFO nesting
    // of two IRQ-saving guards, which crosses their saved IRQ states and leaks
    // an IRQ-disabled state out of this function. That stranded state later
    // trips the atomic-context guard (e.g. `clear_proc_shm` on process exit
    // right after a static-key `disable_key`). Nesting it LIFO here keeps the
    // IRQ flag balanced — this mirrors the kprobe `set_writeable_for_address`
    // path.
    crate::stop_machine::stop_machine(
        move || -> AxResult<()> {
            let mut guard = ax_mm::kernel_aspace().lock();
            if guard.contains_range(aligned_addr, aligned_length) {
                let (_, original_flags, _) = guard.page_table().query(aligned_addr)?;

                guard.protect(
                    aligned_addr,
                    aligned_length,
                    original_flags | MappingFlags::WRITE,
                )?;

                flush_tlb_range(aligned_addr, aligned_length);
                action(addr.as_mut_ptr());

                ax_runtime::hal::cache::clean_dcache_to_pou(addr, len);

                guard.protect(aligned_addr, aligned_length, original_flags)?;
                return Ok(());
            }

            #[cfg(target_arch = "loongarch64")]
            {
                // LoongArch64 kernel text may execute from the 0x9000... DMW
                // direct-map window. DMW translations do not consult PTEs, so
                // there are no page permissions to relax here. Patch directly
                // while all other CPUs are parked, then rely on the per-CPU
                // sync callback to flush instruction state.
                action(addr.as_mut_ptr());
                return Ok(());
            }

            #[cfg(not(target_arch = "loongarch64"))]
            {
                Err(AxError::BadAddress)
            }
        },
        move || sync_modified_kernel_text(aligned_addr, aligned_length),
    )
}

/// Patch many sites within one kernel-text range under a **single**
/// `stop_machine`. `action` runs with `[start, start+len)` made writable and must
/// perform all its writes within that range; the range is restored to its
/// original permissions and instruction-synchronized afterwards.
///
/// This exists for the ftrace function tracer, which arms/disarms thousands of
/// patchable entries at once — doing each through [`patch_kernel_text`] would run
/// one `stop_machine` (parking every core + a TLB/i-cache round trip) per site,
/// which is unusably slow. The whole range must carry uniform original flags
/// (kernel `.text`); callers pass a range bounded by the patch sites.
#[cfg(function_tracer)]
pub fn patch_kernel_text_batch<F>(start: VirtAddr, len: usize, action: F) -> AxResult<()>
where
    F: FnOnce(),
{
    if len == 0 {
        return Ok(());
    }
    let aligned_addr = start.align_down_4k();
    let aligned_length = (start + len).align_up_4k() - aligned_addr;
    // Same LIFO stop_machine / kernel_aspace nesting as `patch_kernel_text`.
    crate::stop_machine::stop_machine(
        move || -> AxResult<()> {
            let mut guard = ax_mm::kernel_aspace().lock();
            if guard.contains_range(aligned_addr, aligned_length) {
                let (_, original_flags, _) = guard.page_table().query(aligned_addr)?;
                guard.protect(
                    aligned_addr,
                    aligned_length,
                    original_flags | MappingFlags::WRITE,
                )?;
                flush_tlb_range(aligned_addr, aligned_length);
                action();
                ax_runtime::hal::cache::clean_dcache_to_pou(start, len);
                guard.protect(aligned_addr, aligned_length, original_flags)?;
                return Ok(());
            }
            #[cfg(target_arch = "loongarch64")]
            {
                action();
                return Ok(());
            }
            #[cfg(not(target_arch = "loongarch64"))]
            {
                Err(AxError::BadAddress)
            }
        },
        move || sync_modified_kernel_text(aligned_addr, aligned_length),
    )
}

/// Writes data to kernel text, ensuring the page permissions are properly handled.
pub fn write_kernel_text(addr: VirtAddr, data: &[u8]) -> AxResult<()> {
    patch_kernel_text(addr, data.len(), |dst| unsafe {
        core::ptr::copy_nonoverlapping(data.as_ptr(), dst, data.len());
    })
}

pub fn flush_tlb_range(start: VirtAddr, size: usize) {
    ax_runtime::hal::cache::flush_tlb_range(start, size);
}

pub fn flush_tlb_range_sync(start: VirtAddr, size: usize) {
    ax_runtime::hal::cache::flush_tlb_range_all_cpus(start, size);
}

fn sync_modified_kernel_text(start: VirtAddr, size: usize) {
    ax_runtime::hal::cache::sync_kernel_text(start, size);
}
