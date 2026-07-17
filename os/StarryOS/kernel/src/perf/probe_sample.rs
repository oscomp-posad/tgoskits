//! `PERF_RECORD_SAMPLE` emission for kprobe / kretprobe / tracepoint / uprobe perf
//! events (`perf record -e kprobe:func`, opened via the raw `perf_event_open`
//! ABI). A probe/tracepoint hit-callback ([`ProbeSampleCallback`]) writes one
//! sample per `sample_period` hits into the event's mmap ring, so a `perf
//! record`-shaped capture produces `perf.data`.
//!
//! The heavy lifting is reused from the hardware-PMU sampling path: the ring is
//! [`super::hw::alloc_sampling_ring`], records are built by
//! [`super::sampling::build_probe_sample`] and published with
//! [`super::sampling::ring_write_process`] (the process/exception-context ring
//! writer — a probe hit fires in a synchronous BRK exception on the interrupted
//! thread, NOT hard-IRQ, so it may allocate/lock and call the process-context
//! writer). The `PollSet`/`IrqNotify` deferred wakeup mirrors the PMU sampler so
//! `perf record`'s `poll()` works.
//!
//! Ring lifetime: the ring pages are owned by the perf process's mmap; the event
//! keeps only a `Weak`, so the callback `upgrade()`s it to pin the pages for each
//! write — a failed upgrade (userspace unmapped the ring) drops the sample rather
//! than risking a use-after-free (same discipline as [`super::syswide`]).

use alloc::sync::{Arc, Weak};
use core::{
    any::Any,
    sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
};

use ax_alloc::GlobalPage;
use ax_errno::{AxError, AxResult};
use ax_kernel_guard::NoPreemptIrqSave;
use ax_kspin::SpinNoIrq;
use ax_memory_addr::PhysAddr;
use ax_task::IrqNotify;
use axpoll::{IoEvents, PollSet};
use kbpf_basic::linux_bpf::perf_event_mmap_page;
use kprobe::{CallBackFunc, PtRegs};

use super::sampling::{
    self, PERF_CONTEXT_KERNEL, PERF_RECORD_MISC_KERNEL, PERF_RECORD_MISC_USER, ProbeSampleData,
    SAMPLE_RECORD_MAX_LEN,
};
use crate::task::AsThread;

/// Per-CPU scratch the callback assembles each `PERF_RECORD_SAMPLE` into, so the
/// record never sits on the exception stack. A probe hit is synchronous and
/// non-nesting per CPU, and the callback holds [`NoPreemptIrqSave`] across the
/// fill + write, so this CPU's buffer is exclusively owned for that window.
#[ax_percpu::def_percpu]
static PROBE_RECORD_SCRATCH: [u8; SAMPLE_RECORD_MAX_LEN] = [0u8; SAMPLE_RECORD_MAX_LEN];

/// `PERF_SAMPLE_CALLCHAIN`: the sample carries a `u64 nr` count then `nr` IPs
/// (a `PERF_CONTEXT_*` marker counts as an entry). Already within the
/// probe-supported set ([`probe_sample_type_supported`]).
const PERF_SAMPLE_CALLCHAIN: u64 = 1 << 5;

/// Max kernel frames captured for a probe-hit callchain: one
/// `PERF_CONTEXT_KERNEL` marker plus up to this many instruction pointers. Kept
/// at the PMU sampler's per-region cap so the assembled record stays within
/// [`SAMPLE_RECORD_MAX_LEN`].
const PROBE_CALLCHAIN_MAX: usize = 64;

/// Sampling state for a probe/tracepoint `perf record` event: the mmap ring the
/// hit-callback writes into, plus the deferred `poll()` wakeup. Shared (`Arc`)
/// between the `ProbePerfEvent`/`TracepointPerfEvent` and its registered callback.
pub struct ProbeSampling {
    /// `attr.sample_type` — the scalar fields each sample carries (validated at
    /// open to be within the probe-supported set).
    sample_type: u64,
    /// One sample emitted per this many hits (`attr.sample_period`, min 1).
    period: u64,
    /// A uprobe hit is user context (`PERF_RECORD_MISC_USER`); kprobe/tracepoint
    /// are kernel (`PERF_RECORD_MISC_KERNEL`).
    is_user: bool,
    /// The probe's address (the sample IP). For a kprobe this is the probed
    /// function's entry (resolved via kallsyms); the interrupted `pt_regs.pc` at
    /// the single-step handler is an out-of-line trampoline address, so — like
    /// Linux — the sample reports the stable probe location instead.
    probe_addr: u64,
    /// Event id for `PERF_SAMPLE_ID` / `IDENTIFIER` (set via `set_id`).
    id: AtomicU64,
    /// The tracepoint `common_type` written into each `PERF_SAMPLE_RAW` record's
    /// first two bytes. `perf`/libtraceevent resolves a raw record to its event
    /// format by this field (`tep_find_event_by_record`), so it must equal the
    /// dynamic event's id; `0` (the default, for a direct `PERF_TYPE_KPROBE` open
    /// with no tracefs event) leaves the record unresolvable, which is fine for the
    /// BPF/hand-rolled paths that never run `perf report`.
    common_type: AtomicU32,
    /// Per-task filter (`perf_event_open` `pid`): the target process pid, or
    /// [`NO_TARGET`] for a system-wide probe (`pid == -1`). A kprobe fires for
    /// every task; when a target is set, [`emit`](Self::emit) drops hits from
    /// other processes so `perf record -- cmd` samples only `cmd` — and, crucially,
    /// not perf's own ring-drain syscalls, which would otherwise storm a hot
    /// global probe (e.g. the syscall dispatcher).
    target_pid: AtomicU64,
    /// Hit counter, for the `period` divisor.
    hits: AtomicU64,
    /// The ring `(Weak<GlobalPage>, ring_vaddr, ring_len)`, set at `device_mmap`.
    /// The strong `Arc` lives in the user VMA; the callback upgrades the `Weak` to
    /// pin the pages for each write.
    ring: SpinNoIrq<Option<(Weak<GlobalPage>, usize, usize)>>,
    /// Deferred POLLIN wakeup: the callback pokes `notify`, the worker wakes
    /// `poll_ready`.
    notify: Arc<IrqNotify>,
    poll_ready: Arc<PollSet>,
    poll_alive: Arc<AtomicBool>,
    /// Whether the deferred notify worker has been spawned (once, at first mmap).
    worker_started: AtomicBool,
}

/// [`ProbeSampling::target_pid`] sentinel for a system-wide probe (no per-task
/// filter). No real process uses this pid, so it can never alias a target.
pub const NO_TARGET: u64 = u64::MAX;

impl ProbeSampling {
    /// Build sampling state for a probe opened with `sample_period > 0`.
    pub fn new(sample_type: u64, sample_period: u64, is_user: bool, probe_addr: u64) -> Arc<Self> {
        Arc::new(Self {
            sample_type,
            period: sample_period.max(1),
            is_user,
            probe_addr,
            id: AtomicU64::new(0),
            common_type: AtomicU32::new(0),
            target_pid: AtomicU64::new(NO_TARGET),
            hits: AtomicU64::new(0),
            ring: SpinNoIrq::new(None),
            notify: Arc::new(IrqNotify::new()),
            poll_ready: Arc::new(PollSet::new()),
            poll_alive: Arc::new(AtomicBool::new(true)),
            worker_started: AtomicBool::new(false),
        })
    }

    /// Record the event id (`set_sample_id`).
    pub fn set_id(&self, id: u64) {
        self.id.store(id, Ordering::Relaxed);
    }

    /// Set the per-task filter from the open `pid`: `Some(p)` samples only
    /// process `p`; `None` (system-wide) samples every hit. Called once at open
    /// before the probe is armed, so a plain `store` needs no ordering dance.
    pub fn set_target_pid(&self, target: Option<u32>) {
        self.target_pid
            .store(target.map_or(NO_TARGET, u64::from), Ordering::Relaxed);
    }

    /// Set the `common_type` id stamped into each raw record so `perf report` can
    /// resolve the event's format. `id` is the dynamic tracepoint event id
    /// (`≤ u16::MAX`); the low 16 bits are what fit the on-wire field.
    pub fn set_common_type(&self, id: u32) {
        self.common_type.store(id, Ordering::Relaxed);
    }

    /// `mmap(perf_fd)`: allocate the ring, store the `Weak`, spawn the deferred
    /// poll-wakeup worker once, and return `(paddr, anchor)` — the anchor is the
    /// sole strong `Arc<GlobalPage>` the caller threads into the user VMA.
    pub fn device_mmap(&self, len: usize) -> AxResult<(PhysAddr, Arc<dyn Any + Send + Sync>)> {
        // Reject a second live mapping (matches HwPerfEvent::device_mmap).
        if let Some((pages, ..)) = self.ring.lock().as_ref()
            && pages.strong_count() > 0
        {
            return Err(AxError::ResourceBusy);
        }
        let (pages, ring_vaddr, paddr) = super::hw::alloc_sampling_ring(len)?;
        *self.ring.lock() = Some((Arc::downgrade(&pages), ring_vaddr, len));
        if !self.worker_started.swap(true, Ordering::AcqRel) {
            super::hw::start_sampling_notify_worker(
                self.poll_ready.clone(),
                self.notify.clone(),
                self.poll_alive.clone(),
            );
        }
        Ok((paddr, pages as Arc<dyn Any + Send + Sync>))
    }

    /// Whether the ring has unread bytes (drives `poll`). Pins the pages while
    /// reading the `perf_event_mmap_page` header; `false` if the ring is absent or
    /// unmapped.
    pub fn has_data(&self) -> bool {
        let guard = self.ring.lock();
        let Some((pages, vaddr, _len)) = guard.as_ref() else {
            return false;
        };
        // Pin the pages for the header read.
        let Some(_pin) = pages.upgrade() else {
            return false;
        };
        if *vaddr == 0 {
            return false;
        }
        // SAFETY: `_pin` keeps the ring pages alive; `vaddr` is the header page.
        let header = *vaddr as *const perf_event_mmap_page;
        let head = unsafe { core::ptr::addr_of!((*header).data_head).read_volatile() };
        let tail = unsafe { core::ptr::addr_of!((*header).data_tail).read_volatile() };
        head != tail
    }

    /// Register the waker for `poll()` readiness.
    pub fn register_poll(&self, waker: &core::task::Waker) {
        // SAFETY: `poll_ready` outlives the registration (owned by this `Arc`);
        // `register` runs in task/deferred context (the `poll` syscall).
        unsafe { self.poll_ready.register(waker, IoEvents::IN) };
    }

    /// Emit one `PERF_RECORD_SAMPLE` (every `period` hits) into the ring.
    /// `callchain` is the optional `PERF_SAMPLE_CALLCHAIN` block (`[PERF_CONTEXT_*,
    /// ip, callers...]`), empty for an event with no interrupted register frame to
    /// unwind (a tracepoint hit). Callable from exception / process / hard-IRQ
    /// context: the ring write is a bounded copy under masked local IRQs, and the
    /// ring pages are pinned via `Weak::upgrade` for the whole write (UAF-safe).
    pub fn emit(&self, callchain: &[u64]) {
        // Attribute to the task that hit the probe. `try_as_thread` is a lock-free
        // downcast; a kernel task with no `Thread` falls back to the scheduler id.
        let curr = ax_task::current();
        let (pid, tid) = match curr.try_as_thread() {
            Some(thr) => (thr.proc_data.proc.pid() as u32, thr.tid()),
            None => {
                let id = curr.id().as_u64() as u32;
                (id, id)
            }
        };
        // Per-task filter: a probe opened for a specific process (`perf record`'s
        // target `cmd`) drops every other process's hits — including perf's own
        // ring-drain/`perf.data`-write syscalls, which would otherwise feed a hot
        // global probe (the syscall dispatcher) back into itself and storm. Done
        // before the period divisor so the period counts only in-target hits.
        let target = self.target_pid.load(Ordering::Relaxed);
        if target != NO_TARGET && u64::from(pid) != target {
            return;
        }
        // Emit one sample per `period` hits.
        let n = self.hits.fetch_add(1, Ordering::Relaxed) + 1;
        if self.period > 1 && !n.is_multiple_of(self.period) {
            return;
        }
        // Snapshot the ring and pin its pages for the whole write (UAF-safe).
        let (pin, ring_vaddr, ring_len) = {
            let guard = self.ring.lock();
            let Some((pages, vaddr, len)) = guard.as_ref() else {
                return;
            };
            let Some(pin) = pages.upgrade() else {
                return;
            };
            (pin, *vaddr, *len)
        };
        if ring_vaddr == 0 {
            return;
        }

        let misc = if self.is_user {
            PERF_RECORD_MISC_USER
        } else {
            PERF_RECORD_MISC_KERNEL
        };

        // `PERF_SAMPLE_RAW` payload — `perf record` sets it by default for a
        // tracepoint event (`-e probe:<kprobe>`). Emit the minimal kprobe record
        // matching the `kprobe_events` `format` file: the four `common_*` header
        // fields then `__probe_ip`. `perf` parses this via the event's own format
        // (loaded from `events/.../format` at open), so `common_type` need not
        // carry the tracepoint id — only `__probe_ip` is referenced by the print
        // fmt. See `sampling::PROBE_RAW_LEN` for the byte layout.
        let mut raw = [0u8; sampling::PROBE_RAW_LEN];
        let raw = if self.sample_type & sampling::PERF_SAMPLE_RAW != 0 {
            // common_type (offset 0, size 2): the event id perf/libtraceevent
            // resolves the record's format by. Must match the tracefs event ID.
            let common_type = self.common_type.load(Ordering::Relaxed) as u16;
            raw[0..2].copy_from_slice(&common_type.to_ne_bytes());
            raw[4..8].copy_from_slice(&tid.to_ne_bytes()); // common_pid (i32)
            raw[8..16].copy_from_slice(&self.probe_addr.to_ne_bytes()); // __probe_ip
            &raw[..]
        } else {
            &raw[..0]
        };

        let data = ProbeSampleData {
            // The stable probe location (kallsyms-symbolizable), not the single-step
            // trampoline pc; `0` for a tracepoint (a hit has no code-address IP).
            ip: self.probe_addr,
            pid,
            tid,
            time: ax_runtime::hal::time::monotonic_time_nanos(),
            cpu: ax_hal::percpu::this_cpu_id() as u32,
            id: self.id.load(Ordering::Relaxed),
            period: self.period,
            callchain,
            raw,
        };

        // Assemble in this CPU's scratch (not the exception stack) and publish.
        // The guard makes the per-CPU scratch exclusive and serializes the write.
        let _guard = NoPreemptIrqSave::new();
        // SAFETY: preemption + local IRQs off, so this CPU's scratch is exclusive.
        let record = unsafe { PROBE_RECORD_SCRATCH.current_ref_mut_raw() };
        let len = sampling::build_probe_sample(&mut record[..], self.sample_type, misc, &data);
        // SAFETY: `pin` keeps the ring pages alive for this write; `ring_vaddr` is
        // the header page of a `ring_len`-byte ring initialized by `device_mmap`.
        unsafe { sampling::ring_write_process(ring_vaddr, ring_len, &record[..len]) };
        drop(_guard);
        drop(pin);
        // Wake the deferred worker so it delivers POLLIN to `perf record`'s poll.
        self.notify.notify_irq();
    }
}

impl core::fmt::Debug for ProbeSampling {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("ProbeSampling")
            .field("sample_type", &self.sample_type)
            .field("period", &self.period)
            .field("is_user", &self.is_user)
            .finish()
    }
}

/// Build sampling state for a probe opened with `sample_period` hits-per-sample
/// (`0` ⇒ no sampling ⇒ `None`, i.e. a BPF-attach / non-sampling probe). Rejects
/// an unsupported `sample_type` at open.
pub fn make_sampling(
    sample_period: u64,
    sample_type: u64,
    is_user: bool,
    probe_addr: u64,
) -> AxResult<Option<Arc<ProbeSampling>>> {
    if sample_period == 0 {
        return Ok(None);
    }
    if !probe_sample_type_supported(sample_type) {
        return Err(AxError::Unsupported);
    }
    Ok(Some(ProbeSampling::new(
        sample_type,
        sample_period,
        is_user,
        probe_addr,
    )))
}

impl Drop for ProbeSampling {
    fn drop(&mut self) {
        // Stop the deferred worker (mirrors HwPerfEvent's Drop).
        self.poll_alive.store(false, Ordering::Release);
        self.notify.notify();
    }
}

/// The callback registered on the probe: on each hit it emits one
/// `PERF_RECORD_SAMPLE` (every `period` hits) into the ring.
pub struct ProbeSampleCallback {
    s: Arc<ProbeSampling>,
}

impl ProbeSampleCallback {
    /// Build the sample-emitting callback for `s` (registered via the probe's
    /// `register_event_callback`).
    pub fn new(s: Arc<ProbeSampling>) -> Arc<dyn CallBackFunc> {
        Arc::new(Self { s })
    }
}

impl CallBackFunc for ProbeSampleCallback {
    fn call(&self, pt_regs: &mut PtRegs) {
        let s = &self.s;
        // Optional kernel callchain (`perf report -g` "who calls this function"):
        // leaf = the stable probe location, callers walked from the interrupted
        // frame pointer (`regs[29]`). Kernel probes only — a uprobe hit's user
        // unwind needs an interrupted SP the BRK frame does not carry on aarch64
        // (`pt_regs.sp == 0`), so user probes emit no chain. Leaf-only unless the
        // kernel keeps frame pointers (same caveat as the PMU callchain); the block
        // layout is what `perf report -g` consumes either way.
        let mut chain = [0u64; 1 + PROBE_CALLCHAIN_MAX];
        let nchain = if s.sample_type & PERF_SAMPLE_CALLCHAIN != 0 && !s.is_user {
            chain[0] = PERF_CONTEXT_KERNEL;
            let fp = pt_regs.regs[29] as usize;
            1 + super::unwind::kernel_callchain(s.probe_addr as usize, fp, &mut chain[1..])
        } else {
            0
        };
        s.emit(&chain[..nchain]);
    }
}

/// Whether a probe `sample_type` is supported. Probe samples carry the scalar
/// fields, an optional callchain, and the optional `PERF_SAMPLE_RAW` tracepoint
/// record; `PERF_SAMPLE_READ` (`1<<4`) / `REGS_USER` (`1<<12`) / `STACK_USER`
/// (`1<<13`) — which need a counter / interrupted user context a probe hit does
/// not have — are rejected (this also keeps the record within the callback's
/// buffer). IP is not required: `perf record` on a tracepoint may omit it.
pub fn probe_sample_type_supported(sample_type: u64) -> bool {
    const PROBE_MASK: u64 = sampling::SUPPORTED_SAMPLE_TYPE & !((1 << 4) | (1 << 12) | (1 << 13));
    sample_type & !PROBE_MASK == 0
}
