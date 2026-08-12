//! Tracepoint perf event. The user attaches a BPF program to a static
//! tracepoint identified by its numeric id (from
//! `/sys/kernel/debug/tracing/events/<sys>/<event>/id`), or — with
//! `sample_period > 0` — captures `perf record`-style `PERF_RECORD_SAMPLE`s
//! (one per hit) into an mmap ring, reusing the probe sampling ring
//! ([`super::probe_sample`]). A tracepoint hit carries no interrupted register
//! frame, so the sample has no code IP (`ip = 0`) and no callchain; it records
//! which task hit the tracepoint, when, and on which CPU (aarch64 only).
//!
//! Adapted from `Starry-OS/StarryOS:ebpf-kmod` (`kernel/src/perf/tracepoint.rs`)
//! to use ktracepoint **0.6**:
//!
//! * ktracepoint 0.6 dropped the `TracePoint<L, K>` lock parameter — there
//!   is a single generic over `K: KernelTraceOps`, and `ExtTracePoint<K>`
//!   wraps callback management.
//! * `TraceEventFunc::new(closure, data)` replaces the trait-object based
//!   `TracePointCallBackFunc::call(entry)` callback registration.
//! * Registration goes through `ExtTracePoint::register(TraceCallbackType::Event(...))`
//!   rather than `TracePoint::register_event_callback(id, callback)`.
//! * Enable/disable is implicit: `ExtTracePoint::register` enables the
//!   static-key when the callback list becomes non-empty.

use alloc::{boxed::Box, sync::Arc};
use core::any::Any;

use ax_errno::{AxError, AxResult};
use ax_memory_addr::PhysAddr;
use axpoll::Pollable;
use kbpf_basic::perf::{PerfProbeArgs, PerfProbeConfig};
use ktracepoint::{TraceCallbackType, TraceEventFunc};

#[cfg(target_arch = "aarch64")]
use super::probe_sample;
use crate::{
    file::FileLike,
    perf::{PerfEventOps, bpf::OwnedEbpfVm},
    tracepoint::{KernelExtTracePoint, lookup_ext_tracepoint},
};

/// Closure signature accepted by `TraceEventFunc::new` for cooked tracepoints:
/// the tracing layer hands over the per-cpu sample bytes plus the type-erased
/// per-callback payload, and the closure dispatches into the BPF VM.
type TpCallback = Box<dyn Fn(&[u8], &(dyn Any + Send + Sync)) + Send + Sync>;

/// Per-fd tracepoint perf event. Holds the Arc<Mutex<ExtTracePoint>> so we
/// can register/unregister callbacks on drop; remembers the callback
/// payload so the same registration can be undone (ktracepoint 0.6
/// `unregister(callback)` compares Arc pointer identity).
pub struct TracepointPerfEvent {
    _args: PerfProbeArgs,
    ext_tp: KernelExtTracePoint,
    registered: alloc::vec::Vec<Arc<TraceEventFunc>>,
    /// Sampling state for `perf record -e <tracepoint>` (`sample_period > 0`): the
    /// mmap ring the hit-callback writes `PERF_RECORD_SAMPLE` into. `None` for a
    /// BPF-attach or non-sampling tracepoint.
    #[cfg(target_arch = "aarch64")]
    sampling: Option<Arc<probe_sample::ProbeSampling>>,
}

impl core::fmt::Debug for TracepointPerfEvent {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("TracepointPerfEvent").finish()
    }
}

impl TracepointPerfEvent {
    /// Create a perf event for the given resolved tracepoint.
    pub fn new(args: PerfProbeArgs, ext_tp: KernelExtTracePoint) -> Self {
        Self {
            _args: args,
            ext_tp,
            registered: alloc::vec::Vec::new(),
            #[cfg(target_arch = "aarch64")]
            sampling: None,
        }
    }

    /// Register a sample-emitting callback on the tracepoint when it was opened
    /// with `sample_period > 0` (`perf record -e <tracepoint>`). Each hit writes a
    /// `PERF_RECORD_SAMPLE` (every `sample_period` hits) into the event's ring. The
    /// callback is a cooked `TraceEventFunc` starting disabled — `enable()` flips
    /// its `perf_enable` flag alongside any BPF callbacks.
    #[cfg(target_arch = "aarch64")]
    fn attach_sampling(&mut self, sample_period: u64, sample_type: u64) -> AxResult<()> {
        // A tracepoint hit is kernel context with no code IP to sample
        // (`is_user = false`, `probe_addr = 0`).
        let Some(s) = probe_sample::make_sampling(sample_period, sample_type, false, 0)? else {
            return Ok(());
        };
        let s_cb = s.clone();
        let func: TpCallback = Box::new(move |_entry, _data| {
            // A tracepoint hit carries no interrupted register frame, so no
            // callchain — emit the scalar fields (tid / time / cpu) only.
            s_cb.emit(&[]);
        });
        // No per-callback payload — the closure captures its `ProbeSampling`. The
        // tracing layer stores the data as `Box<dyn Any + Send + Sync>`.
        let callback = Arc::new(TraceEventFunc::new(func, Box::new(())));
        self.ext_tp
            .lock()
            .register(TraceCallbackType::Event(callback.clone()));
        self.registered.push(callback);
        self.sampling = Some(s);
        Ok(())
    }
}

impl Pollable for TracepointPerfEvent {
    fn poll(&self) -> axpoll::IoEvents {
        // A sampling tracepoint (`perf record`) is readable when its ring has
        // unread bytes. A BPF-attach tracepoint delivers no fd readiness — its
        // output goes through the attached BPF program / trace_pipe.
        #[cfg(target_arch = "aarch64")]
        if let Some(s) = &self.sampling {
            return if s.has_data() {
                axpoll::IoEvents::IN
            } else {
                axpoll::IoEvents::empty()
            };
        }
        axpoll::IoEvents::empty()
    }

    fn register(&self, context: &mut core::task::Context<'_>, events: axpoll::IoEvents) {
        #[cfg(target_arch = "aarch64")]
        if let Some(s) = &self.sampling {
            s.register_poll(context.waker());
            return;
        }
        // A BPF-attach tracepoint delivers no fd readiness through poll;
        // sample delivery is via the attached BPF program or trace_pipe.
        let _ = (context, events);
    }
}

impl PerfEventOps for TracepointPerfEvent {
    fn set_bpf_prog(&mut self, bpf_prog: Arc<dyn FileLike>) -> AxResult<()> {
        // `OwnedEbpfVm` bundles the rbpf interpreter with the `Arc<BpfProg>`
        // that backs its instruction slice (drop order is field-order, so
        // the borrower dies before the buffer). `execute_program` runs off
        // `&self`, so the VM is driven directly from the `&dyn Any` the
        // `TraceEventFunc` closure receives — no lock required.
        struct Ctx {
            vm: OwnedEbpfVm,
        }
        let ctx = Box::new(Ctx {
            vm: OwnedEbpfVm::new(bpf_prog)?,
        });

        let func: TpCallback = Box::new(|entry: &[u8], data: &(dyn Any + Send + Sync)| {
            // `TraceEventFunc` keeps the payload as `Box<dyn Any + Send + Sync>`
            // and hands the closure `&self.data`, so the concrete type observed
            // here is the *box*, not `Ctx` (same as the raw-tracepoint path in
            // `raw_tracepoint.rs`). Downcast through the box first.
            let ctx = data
                .downcast_ref::<Box<dyn Any + Send + Sync>>()
                .and_then(|boxed| boxed.downcast_ref::<Ctx>())
                .expect("tracepoint Ctx mismatch");
            // BPF programs expect a mutable context slice; the
            // tracepoint hands us a `&[u8]` carved out of its
            // per-cpu sample buffer, which is single-writer at that
            // point, so casting to `&mut [u8]` is safe under the
            // tracepoint contract.
            let entry =
                unsafe { core::slice::from_raw_parts_mut(entry.as_ptr() as *mut u8, entry.len()) };
            if let Err(e) = ctx.vm.execute_program(entry) {
                error!("tracepoint BPF program failed: {e:?}");
            }
        });
        let callback = Arc::new(TraceEventFunc::new(func, ctx));
        self.ext_tp
            .lock()
            .register(TraceCallbackType::Event(callback.clone()));
        self.registered.push(callback);
        Ok(())
    }

    fn enable(&mut self) -> AxResult<()> {
        // ktracepoint dispatch only invokes a cooked `TraceEventFunc` when
        // its per-callback `perf_enabled` flag is set (see ktracepoint 0.6
        // `basic_macro.rs`), and `TraceEventFunc::new` starts disabled. So a
        // perf event that is registered but not enabled would silently never
        // fire — we must flip the flag on every callback we registered.
        for cb in &self.registered {
            cb.set_perf_enable(true);
        }
        Ok(())
    }

    fn disable(&mut self) -> AxResult<()> {
        for cb in &self.registered {
            cb.set_perf_enable(false);
        }
        Ok(())
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    /// `mmap(perf_fd)`: allocate the sampling ring for a `perf record` tracepoint.
    /// For a BPF-attach or non-sampling tracepoint (no ring), `mmap` stays
    /// unsupported.
    fn device_mmap(&mut self, len: usize) -> AxResult<(PhysAddr, Arc<dyn Any + Send + Sync>)> {
        #[cfg(target_arch = "aarch64")]
        if let Some(s) = &self.sampling {
            return s.device_mmap(len);
        }
        let _ = len;
        Err(AxError::Unsupported)
    }

    /// Record the event id a tracepoint sample carries in `PERF_SAMPLE_ID` /
    /// `IDENTIFIER`. No-op for a non-sampling tracepoint.
    fn set_sample_id(&mut self, id: u64) {
        #[cfg(target_arch = "aarch64")]
        if let Some(s) = &self.sampling {
            s.set_id(id);
            return;
        }
        let _ = id;
    }
}

impl Drop for TracepointPerfEvent {
    fn drop(&mut self) {
        let mut ext_tp = self.ext_tp.lock();
        for cb in self.registered.drain(..) {
            ext_tp.unregister(TraceCallbackType::Event(cb));
        }
    }
}

/// Build a tracepoint perf event from `perf_event_open` args. The config
/// field carries the numeric tracepoint id (the same value debugfs
/// `events/<sys>/<event>/id` reports).
///
/// When `sample_period > 0` (`perf record -e <tracepoint>`), a sample-emitting
/// callback is attached so each hit writes a `PERF_RECORD_SAMPLE` into the
/// event's ring (aarch64 only); otherwise the tracepoint stays BPF-attach-only.
pub fn perf_event_open_tracepoint(
    args: PerfProbeArgs,
    sample_period: u64,
    sample_type: u64,
) -> AxResult<TracepointPerfEvent> {
    let tp_id = match args.config {
        PerfProbeConfig::Raw(id) => id as u32,
        _ => return Err(AxError::InvalidInput),
    };
    let ext_tp = lookup_ext_tracepoint(tp_id).ok_or(AxError::NotFound)?;
    #[cfg_attr(not(target_arch = "aarch64"), allow(unused_mut))]
    let mut ev = TracepointPerfEvent::new(args, ext_tp);
    #[cfg(target_arch = "aarch64")]
    ev.attach_sampling(sample_period, sample_type)?;
    #[cfg(not(target_arch = "aarch64"))]
    let _ = (sample_period, sample_type);
    Ok(ev)
}
