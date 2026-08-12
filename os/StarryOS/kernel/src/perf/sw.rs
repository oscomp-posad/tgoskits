//! Software perf events (`PERF_TYPE_SOFTWARE`) as real per-task counters.
//!
//! `perf stat -- cmd` opens its default set with no `-e`: the hardware
//! `cycles`/`instructions` plus five *software* events — `cpu-clock`,
//! `task-clock`, `context-switches`, `cpu-migrations`, `page-faults`. Those five
//! used to dispatch to the BPF stub ([`super::bpf::BpfPerfEventWrapper`]), which
//! has no readable count, so `read(perf_fd)` returned `Unsupported` and every
//! default row printed `<not counted>`. This module makes them real per-task
//! counters so a bare `perf stat -- cmd` looks correct.
//!
//! Each event is a lightweight [`SwPerTaskCounter`] attached to the monitored
//! [`Thread`], driven by cheap hooks:
//!
//! * [`sched_in`] / [`sched_out`] — called from the task scope enter/leave hooks
//!   (the same switch path as the hardware [`super::task::perf_sched_in`]); they
//!   accrue `task-clock` on-CPU time, count `context-switches` (one per
//!   deschedule) and `cpu-migrations` (a slice on a different core than the last).
//! * [`on_page_fault`] — called from the user page-fault handler; counts
//!   `page-faults` for the faulting thread.
//!
//! `cpu-clock` needs no hook: it is wall-clock time while the event is enabled.
//!
//! All hooks early-out on a single relaxed load of [`PERF_SW_ACTIVE`] when no
//! software event exists anywhere, so there is no cost on the hot paths in the
//! common case (mirrors [`super::task::PERF_TASK_ACTIVE`]). Mutation is entirely
//! through atomics, so the counter is `Sync` and the hooks need no allocation.

use alloc::{sync::Arc, vec::Vec};
use core::{
    any::Any,
    sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering},
    task::Context,
};

use ax_errno::{AxError, AxResult};
use ax_kspin::SpinNoIrq;
use axpoll::{IoEvents, Pollable};
use kbpf_basic::linux_bpf::{perf_event_attr, perf_sw_ids};

use super::{PerfEventOps, PerfReadValues};
use crate::task::{AsThread, Thread};

/// Number of live software counters process-wide. The scheduler + fault hooks
/// early-out when this is zero, so there is no cost on those hot paths when no
/// software perf event exists. Incremented at open, decremented when the owning
/// fd drops.
static PERF_SW_ACTIVE: AtomicUsize = AtomicUsize::new(0);

/// Number of live *system-wide* (`pid < 0`) software counters — a separate gate
/// from [`PERF_SW_ACTIVE`]. The scheduler + fault hooks bump the global
/// [`SYS_SW`] counters for EVERY task's switch/fault, so a `perf stat/top -a`
/// software event aggregates machine-wide. `0` ⇒ the hooks skip the global path.
static PERF_SYS_SW_ACTIVE: AtomicUsize = AtomicUsize::new(0);

/// The system-wide (`-a`) software counters, aggregated across all tasks by the
/// hooks. Small (one per open `-a` software event); the `SpinNoIrq` is taken on
/// the switch/fault hot path only while [`PERF_SYS_SW_ACTIVE`] is nonzero.
static SYS_SW: SpinNoIrq<Vec<Arc<SwPerTaskCounter>>> = SpinNoIrq::new(Vec::new());

/// Sentinel for [`SwPerTaskCounter::last_cpu`] before the first slice, so the
/// first `sched_in` does not falsely count a migration.
const CPU_UNSET: u32 = u32::MAX;

#[inline]
fn now_ns() -> u64 {
    ax_runtime::hal::time::monotonic_time_nanos()
}

/// The five software events implemented as counters.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SwId {
    /// `PERF_COUNT_SW_CPU_CLOCK`: wall-clock ns while enabled.
    CpuClock,
    /// `PERF_COUNT_SW_TASK_CLOCK`: ns the task actually ran while enabled.
    TaskClock,
    /// `PERF_COUNT_SW_PAGE_FAULTS`: user page faults taken by the task.
    PageFaults,
    /// `PERF_COUNT_SW_CONTEXT_SWITCHES`: times the task was descheduled.
    ContextSwitches,
    /// `PERF_COUNT_SW_CPU_MIGRATIONS`: times the task resumed on a new core.
    CpuMigrations,
}

impl SwId {
    /// Maps the `perf_sw_ids` config to a counter kind, or `None` for software
    /// ids this module does not implement (e.g. `PERF_COUNT_SW_DUMMY`, which
    /// `perf record` uses for its side-band tracking event and which stays on the
    /// BPF/ring path).
    fn from_raw(id: perf_sw_ids) -> Option<Self> {
        Some(match id {
            perf_sw_ids::PERF_COUNT_SW_CPU_CLOCK => SwId::CpuClock,
            perf_sw_ids::PERF_COUNT_SW_TASK_CLOCK => SwId::TaskClock,
            perf_sw_ids::PERF_COUNT_SW_PAGE_FAULTS => SwId::PageFaults,
            perf_sw_ids::PERF_COUNT_SW_CONTEXT_SWITCHES => SwId::ContextSwitches,
            perf_sw_ids::PERF_COUNT_SW_CPU_MIGRATIONS => SwId::CpuMigrations,
            _ => return None,
        })
    }
}

/// Returns `true` if `id` is a software event this module implements as a real
/// counter (so the dispatcher routes it here instead of the BPF stub).
pub fn is_counting_sw(id: perf_sw_ids) -> bool {
    SwId::from_raw(id).is_some()
}

/// One software counter bound to a specific task.
///
/// Interior-mutable and allocation-free (atomics only) so the scheduler and
/// fault hooks can drive it from IRQ-off / hot-path context.
#[derive(Debug)]
pub struct SwPerTaskCounter {
    kind: SwId,
    /// Opened system-wide (`pid < 0`): the hooks aggregate it across ALL tasks via
    /// [`SYS_SW`], `cpu-clock` counts every online CPU's wall time, and it lives in
    /// the global registry rather than a thread's list.
    system_wide: bool,
    /// `attr.read_format`, controlling which fields `read(perf_fd)` emits.
    read_format: u64,
    /// Userspace wants this event counting (`!disabled` at open or after
    /// `ioctl(ENABLE)`). Hooks and `read` ignore a disabled counter.
    enabled: AtomicBool,
    /// The owning fd has closed; hooks stop touching this counter and it may be
    /// reaped from the thread's list.
    dead: AtomicBool,
    /// Event count for the discrete events (page-faults / context-switches /
    /// cpu-migrations).
    count: AtomicU64,
    /// Accumulated on-CPU time for `task-clock` (ns).
    runtime_ns: AtomicU64,
    /// Monotonic ns this task last became on-CPU with the event enabled, or `0`
    /// when off-CPU; the base for the in-flight `task-clock` slice.
    run_since_ns: AtomicU64,
    /// Accumulated wall time the event has been enabled across past windows (ns).
    time_enabled_ns: AtomicU64,
    /// Monotonic ns the current enabled window opened; valid iff `enabled`.
    enabled_since_ns: AtomicU64,
    /// Logical CPU id of the last slice, for `cpu-migrations`. `CPU_UNSET` until
    /// the first `sched_in`.
    last_cpu: AtomicU32,
}

impl SwPerTaskCounter {
    fn new(kind: SwId, attr: &perf_event_attr, system_wide: bool) -> Self {
        // Enable at open unless the event is opened disabled *and* not armed to
        // start on exec. `perf stat -- cmd` opens with enable_on_exec; treat that
        // as enable-at-open (the pre-exec window is negligible) so counts appear
        // without a dedicated exec hook.
        let enabled = attr.disabled() == 0 || attr.enable_on_exec() != 0;
        let now = now_ns();
        Self {
            kind,
            system_wide,
            read_format: attr.read_format,
            enabled: AtomicBool::new(enabled),
            dead: AtomicBool::new(false),
            count: AtomicU64::new(0),
            runtime_ns: AtomicU64::new(0),
            run_since_ns: AtomicU64::new(0),
            time_enabled_ns: AtomicU64::new(0),
            enabled_since_ns: AtomicU64::new(if enabled { now } else { 0 }),
            last_cpu: AtomicU32::new(CPU_UNSET),
        }
    }

    fn enable(&self) {
        if !self.enabled.swap(true, Ordering::AcqRel) {
            self.enabled_since_ns.store(now_ns(), Ordering::Release);
        }
    }

    fn disable(&self) {
        if self.enabled.swap(false, Ordering::AcqRel) {
            let now = now_ns();
            // Close the enabled wall-time window.
            let since = self.enabled_since_ns.load(Ordering::Acquire);
            self.time_enabled_ns
                .fetch_add(now.saturating_sub(since), Ordering::AcqRel);
            // Fold any in-flight task-clock slice (the task may be running now).
            let run_since = self.run_since_ns.swap(0, Ordering::AcqRel);
            if run_since != 0 {
                self.runtime_ns
                    .fetch_add(now.saturating_sub(run_since), Ordering::AcqRel);
            }
        }
    }

    fn reset(&self) {
        self.count.store(0, Ordering::Release);
        self.runtime_ns.store(0, Ordering::Release);
        self.time_enabled_ns.store(0, Ordering::Release);
        self.run_since_ns.store(0, Ordering::Release);
        if self.enabled.load(Ordering::Acquire) {
            self.enabled_since_ns.store(now_ns(), Ordering::Release);
        }
    }

    fn snapshot(&self) -> PerfReadValues {
        let now = now_ns();
        let enabled = self.enabled.load(Ordering::Acquire);
        let time_enabled = self.time_enabled_ns.load(Ordering::Acquire)
            + if enabled {
                now.saturating_sub(self.enabled_since_ns.load(Ordering::Acquire))
            } else {
                0
            };
        let value = match self.kind {
            // `perf stat -a` cpu-clock is the summed wall time across every online
            // CPU (each contributes its own clock), so scale by the CPU count; a
            // per-task cpu-clock is just the enabled wall time.
            SwId::CpuClock if self.system_wide => time_enabled * ax_hal::cpu_num() as u64,
            SwId::CpuClock => time_enabled,
            SwId::TaskClock => {
                let run_since = self.run_since_ns.load(Ordering::Acquire);
                self.runtime_ns.load(Ordering::Acquire)
                    + if enabled && run_since != 0 {
                        now.saturating_sub(run_since)
                    } else {
                        0
                    }
            }
            _ => self.count.load(Ordering::Acquire),
        };
        PerfReadValues {
            value,
            time_enabled,
            // No multiplexing for software counters: running == enabled.
            time_running: time_enabled,
            read_format: self.read_format,
            lost: 0,
        }
    }
}

/// `PERF_TYPE_SOFTWARE` counting event handle returned by `perf_event_open(2)`.
#[derive(Debug)]
pub struct SwPerfEvent {
    ctr: Arc<SwPerTaskCounter>,
}

impl Drop for SwPerfEvent {
    fn drop(&mut self) {
        // Mark dead (hooks stop touching it) and release the global gate exactly
        // once. The counter's `Arc` may linger in the thread's list until the
        // next open reaps it, or until the thread exits.
        if !self.ctr.dead.swap(true, Ordering::AcqRel) {
            if self.ctr.system_wide {
                PERF_SYS_SW_ACTIVE.fetch_sub(1, Ordering::AcqRel);
            } else {
                PERF_SW_ACTIVE.fetch_sub(1, Ordering::AcqRel);
            }
        }
    }
}

impl PerfEventOps for SwPerfEvent {
    fn enable(&mut self) -> AxResult<()> {
        self.ctr.enable();
        Ok(())
    }

    fn disable(&mut self) -> AxResult<()> {
        self.ctr.disable();
        Ok(())
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    fn read_values(&mut self) -> AxResult<PerfReadValues> {
        Ok(self.ctr.snapshot())
    }

    fn reset(&mut self) -> AxResult<()> {
        self.ctr.reset();
        Ok(())
    }
}

impl Pollable for SwPerfEvent {
    fn poll(&self) -> IoEvents {
        // A counting event is always readable: `read(perf_fd)` returns the
        // current value without blocking.
        IoEvents::IN
    }

    fn register(&self, _context: &mut Context<'_>, _events: IoEvents) {
        // Nothing to wake: the value is always immediately available.
    }
}

/// Attach `ctr` to `thr`'s software-counter list, reaping any dead entries left
/// by closed fds, and bump the global gate.
fn attach(thr: &Thread, ctr: Arc<SwPerTaskCounter>) {
    let mut list = thr.perf_sw_counters.lock();
    list.retain(|c| !c.dead.load(Ordering::Acquire));
    list.push(ctr);
    PERF_SW_ACTIVE.fetch_add(1, Ordering::AcqRel);
}

/// Register a *system-wide* software counter in [`SYS_SW`], reaping dead entries
/// left by closed fds, and bump the global gate.
fn attach_sys(ctr: Arc<SwPerTaskCounter>) {
    let mut list = SYS_SW.lock();
    list.retain(|c| !c.dead.load(Ordering::Acquire));
    list.push(ctr);
    PERF_SYS_SW_ACTIVE.fetch_add(1, Ordering::AcqRel);
}

/// Add `n` to every enabled system-wide counter of `kind` — the machine-wide
/// aggregation the switch / fault hooks feed. Called only while
/// [`PERF_SYS_SW_ACTIVE`] is nonzero.
fn sys_bump(kind: SwId, n: u64) {
    let list = SYS_SW.lock();
    for c in list.iter() {
        if c.kind == kind && !c.dead.load(Ordering::Acquire) && c.enabled.load(Ordering::Acquire) {
            c.count.fetch_add(n, Ordering::Relaxed);
        }
    }
}

/// Open a `PERF_TYPE_SOFTWARE` counting event.
///
/// * `pid > 0` — a specific tid; `pid == 0` — the caller: a per-task counter in
///   that thread's list (`task-clock` / `cpu-migrations` etc. all accurate).
/// * `pid < 0` — system-wide (`perf stat/top -a`): a global counter the hooks
///   aggregate across all tasks. `cpu-clock` (wall time × online CPUs),
///   `context-switches` and `page-faults` are accurate machine-wide;
///   `task-clock` and `cpu-migrations` — which need per-task state — read `0` in
///   v1 (`cpu-clock` is the default `-a` clock event, so `perf stat -a` is
///   correct for its standard software rows).
pub fn perf_event_open_sw(
    attr: &perf_event_attr,
    sw_id: perf_sw_ids,
    pid: i32,
) -> AxResult<SwPerfEvent> {
    let kind = SwId::from_raw(sw_id).ok_or(AxError::Unsupported)?;
    let system_wide = pid < 0;
    let ctr = Arc::new(SwPerTaskCounter::new(kind, attr, system_wide));

    if pid > 0 {
        let task = crate::task::get_task(pid as u32)?;
        let thr = task.try_as_thread().ok_or(AxError::NoSuchProcess)?;
        attach(thr, ctr.clone());
    } else if pid == 0 {
        let curr = ax_task::current();
        let thr = curr.try_as_thread().ok_or(AxError::NoSuchProcess)?;
        attach(thr, ctr.clone());
    } else {
        attach_sys(ctr.clone());
    }

    Ok(SwPerfEvent { ctr })
}

/// Scheduler hook: `thr` is about to start running on this CPU. Opens the
/// `task-clock` slice and counts a `cpu-migrations` event when the core changed.
pub fn sched_in(thr: &Thread) {
    if PERF_SW_ACTIVE.load(Ordering::Acquire) == 0 {
        return;
    }
    let list = thr.perf_sw_counters.lock();
    if list.is_empty() {
        return;
    }
    let now = now_ns();
    let this_cpu = ax_hal::percpu::this_cpu_id() as u32;
    for c in list.iter() {
        if c.dead.load(Ordering::Acquire) || !c.enabled.load(Ordering::Acquire) {
            continue;
        }
        match c.kind {
            SwId::TaskClock => c.run_since_ns.store(now, Ordering::Release),
            SwId::CpuMigrations => {
                let last = c.last_cpu.swap(this_cpu, Ordering::AcqRel);
                if last != CPU_UNSET && last != this_cpu {
                    c.count.fetch_add(1, Ordering::Relaxed);
                }
            }
            _ => {}
        }
    }
}

/// Scheduler hook: `thr` is about to stop running on this CPU. Folds the
/// `task-clock` slice and counts a `context-switches` event (one per deschedule).
pub fn sched_out(thr: &Thread) {
    let sys = PERF_SYS_SW_ACTIVE.load(Ordering::Acquire) != 0;
    if PERF_SW_ACTIVE.load(Ordering::Acquire) == 0 && !sys {
        return;
    }
    // System-wide: one machine-wide context-switch per deschedule of any task.
    if sys {
        sys_bump(SwId::ContextSwitches, 1);
    }
    let list = thr.perf_sw_counters.lock();
    if list.is_empty() {
        return;
    }
    let now = now_ns();
    for c in list.iter() {
        if c.dead.load(Ordering::Acquire) || !c.enabled.load(Ordering::Acquire) {
            continue;
        }
        match c.kind {
            SwId::TaskClock => {
                let run_since = c.run_since_ns.swap(0, Ordering::AcqRel);
                if run_since != 0 {
                    c.runtime_ns
                        .fetch_add(now.saturating_sub(run_since), Ordering::AcqRel);
                }
            }
            SwId::ContextSwitches => {
                c.count.fetch_add(1, Ordering::Relaxed);
            }
            _ => {}
        }
    }
}

/// Fault hook: `thr` just took a user page fault. Counts a `page-faults` event.
pub fn on_page_fault(thr: &Thread) {
    let sys = PERF_SYS_SW_ACTIVE.load(Ordering::Acquire) != 0;
    if PERF_SW_ACTIVE.load(Ordering::Acquire) == 0 && !sys {
        return;
    }
    // System-wide: one machine-wide page-fault per user fault of any task.
    if sys {
        sys_bump(SwId::PageFaults, 1);
    }
    let list = thr.perf_sw_counters.lock();
    for c in list.iter() {
        if c.kind == SwId::PageFaults
            && !c.dead.load(Ordering::Acquire)
            && c.enabled.load(Ordering::Acquire)
        {
            c.count.fetch_add(1, Ordering::Relaxed);
        }
    }
}
