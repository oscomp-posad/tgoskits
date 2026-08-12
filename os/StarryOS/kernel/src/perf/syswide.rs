//! System-wide (`perf record -a`) side-band subscriber registry.
//!
//! The per-task side-band hooks ([`super::task::on_exec_sideband`] etc.) emit
//! `PERF_RECORD_COMM` / `MMAP2` / `FORK` / `EXIT` into each *monitored thread's*
//! own event ring. A `perf record -a` capture instead opens one sampling event
//! **per CPU** (`home_cpu == cpu`, see the `-a` fan-out in [`super::hw`]); its ring
//! must also receive those side-band records for whatever process runs on that
//! core, or `perf report` cannot symbolize a system-wide profile.
//!
//! Because each `-a` ring is pinned to a core and the side-band hooks run in
//! **process context on the core the syscall executes**, a hook writes the LOCAL
//! core's `-a` ring directly, via [`super::sampling::ring_write_process`] (which
//! masks local IRQs to serialize against that ring's overflow handler on the same
//! core). No cross-core write, no IPI, no extra lock is needed — the subscriber
//! lives in a per-CPU cell registered on the ring's `home_cpu`.
//!
//! ## Ring lifetime
//!
//! The `-a` ring is owned by the *perf process's* mmap, not the task that trips a
//! side-band hook, so it can be unmapped concurrently. [`RingState`] keeps only a
//! `Weak` to the pages ([super::hw]); a hook therefore snapshots the per-CPU entry
//! under a guard, releases it, then `upgrade()`s that `Weak` to pin the pages for
//! the duration of the write. A failed upgrade means userspace already unmapped
//! the ring, so the record is dropped — never a use-after-free.
//!
//! ## Pre-existing maps
//!
//! `perf record -a` synthesizes COMM + MMAP2 for already-running processes in
//! userspace from `/proc/<pid>/maps`; the kernel only emits records for activity
//! *during* the trace (new exec / mmap / clone / exit), so no at-open synthesis
//! happens here (v1).

use alloc::sync::Weak;
use core::sync::atomic::{AtomicUsize, Ordering};

use ax_alloc::GlobalPage;
use ax_kernel_guard::NoPreemptIrqSave;

use super::sideband::SidebandTarget;

/// One system-wide `-a` ring subscribed to side-band records, registered on the
/// ring's `home_cpu`. Not `Copy` (it holds a `Weak` to the ring pages), so the
/// per-CPU cell is cloned out under a guard rather than bit-copied.
#[derive(Clone)]
pub struct SysSidebandEntry {
    /// Weak handle to the `-a` ring's pages (strong refs live in the perf
    /// process's mmap VMA). Upgraded to a strong `Arc` for the duration of a
    /// side-band write so the pages cannot be freed underneath it; a failed
    /// upgrade means the ring was unmapped, so the write is skipped.
    pub pages: Weak<GlobalPage>,
    /// Kernel vaddr of the ring's header page.
    pub ring_vaddr: usize,
    /// Total ring length in bytes.
    pub ring_len: usize,
    /// `attr.sample_type` — selects the `sample_id_all` trailer fields.
    pub sample_type: u64,
    /// `attr.sample_id_all`.
    pub sample_id_all: bool,
    /// The event id (for the trailer's `ID` / `IDENTIFIER` fields).
    pub id: u64,
    /// `attr.comm` — emit `PERF_RECORD_COMM`.
    pub want_comm: bool,
    /// `attr.mmap2` — emit `PERF_RECORD_MMAP2`.
    pub want_mmap2: bool,
    /// `attr.task` — emit `PERF_RECORD_FORK` / `EXIT`.
    pub want_task: bool,
}

/// Per-CPU system-wide side-band subscriber. `perf record -a` opens one sampling
/// event per CPU (`home_cpu == cpu`); each registers here on its own core, so a
/// side-band hook running on core `H` finds core `H`'s `-a` ring. `None` when no
/// system-wide capture owns this core. v1 supports one subscriber per core.
#[ax_percpu::def_percpu]
static SYS_SIDEBAND: Option<SysSidebandEntry> = None;

/// Number of registered system-wide side-band subscribers. Lets the per-task
/// side-band hooks — gated on `PERF_TASK_ACTIVE` — also run when *only* a
/// system-wide capture is active. Bumped by [`register`], dropped by
/// [`unregister`].
static SYS_SIDEBAND_ACTIVE: AtomicUsize = AtomicUsize::new(0);

/// Whether any system-wide side-band subscriber is registered — the fast gate the
/// side-band hooks add to their `PERF_TASK_ACTIVE` check.
pub fn active() -> bool {
    SYS_SIDEBAND_ACTIVE.load(Ordering::Acquire) != 0
}

/// Register (or replace) the current core's system-wide side-band subscriber.
///
/// Runs on the ring's `home_cpu` at event enable ([`super::hw`]), in process or
/// IPI-arm context; the [`NoPreemptIrqSave`] guard makes it exclusive against the
/// local side-band hooks and [`unregister`] on this core.
pub fn register(entry: SysSidebandEntry) {
    let _guard = NoPreemptIrqSave::new();
    // SAFETY: preemption + local IRQs are off, so this CPU's cell is exclusive.
    let cell = unsafe { SYS_SIDEBAND.current_ref_mut_raw() };
    if cell.is_none() {
        SYS_SIDEBAND_ACTIVE.fetch_add(1, Ordering::AcqRel);
    }
    *cell = Some(entry);
}

/// Clear the current core's system-wide side-band subscriber. Idempotent; runs on
/// `home_cpu` at sampling teardown ([`super::hw`]). Mirror of [`register`].
pub fn unregister() {
    let _guard = NoPreemptIrqSave::new();
    // SAFETY: see `register`.
    let cell = unsafe { SYS_SIDEBAND.current_ref_mut_raw() };
    if cell.is_some() {
        SYS_SIDEBAND_ACTIVE.fetch_sub(1, Ordering::AcqRel);
    }
    *cell = None;
}

/// The current core's `-a` subscriber, exposed to a side-band hook: a
/// [`SidebandTarget`] to write plus which record kinds it wants.
pub struct SysSidebandView<'a> {
    /// Where to write, with `pid`/`tid` filled in for the originating task.
    pub target: &'a SidebandTarget,
    /// Whether this subscriber wants `PERF_RECORD_COMM`.
    pub want_comm: bool,
    /// Whether this subscriber wants `PERF_RECORD_MMAP2`.
    pub want_mmap2: bool,
    /// Whether this subscriber wants `PERF_RECORD_FORK` / `EXIT`.
    pub want_task: bool,
}

/// Run `f` with a [`SysSidebandView`] for the current core's `-a` subscriber, if
/// one is registered and its ring is still mapped.
///
/// Snapshots the per-CPU entry under a short guard, releases it, then `upgrade()`s
/// the ring's `Weak` to pin the pages for the whole call (a failed upgrade —
/// userspace unmapped the ring — skips `f`). `f` builds records for the given
/// `pid`/`tid` and emits whichever the subscriber's `want_*` flags select, into
/// `view.target`, which stays valid because the pinned pages outlive `f`. Runs on
/// the emitting core in process context (`f` may allocate and take the aspace
/// lock; it must not sleep).
pub fn with_local_target<F>(pid: u32, tid: u32, f: F)
where
    F: FnOnce(&SysSidebandView<'_>),
{
    let entry = {
        let _guard = NoPreemptIrqSave::new();
        // Clone out (a `Weak` bump + scalars) so `f` — which allocates and writes
        // the ring — runs without holding the guard.
        // SAFETY: preemption + local IRQs are off, so this CPU's cell is exclusive.
        unsafe { SYS_SIDEBAND.current_ref_raw().clone() }
    };
    let Some(entry) = entry else {
        return;
    };
    // Pin the ring pages for the write. The strong refs live in the perf process's
    // mmap, so this succeeds while the ring is mapped and fails once unmapped.
    let Some(_pages) = entry.pages.upgrade() else {
        return;
    };
    let target = SidebandTarget {
        ring_vaddr: entry.ring_vaddr,
        ring_len: entry.ring_len,
        sample_type: entry.sample_type,
        sample_id_all: entry.sample_id_all,
        id: entry.id,
        pid,
        tid,
    };
    let view = SysSidebandView {
        target: &target,
        want_comm: entry.want_comm,
        want_mmap2: entry.want_mmap2,
        want_task: entry.want_task,
    };
    f(&view);
    // `_pages` (the pinning strong `Arc`) drops here, after all ring writes in `f`.
}
