//! Kprobe / kretprobe perf events. Owns a registered probe + the list of
//! callback ids it has attached to the probe, so `Drop` can detach.
//!
//! Ported from `Starry-OS/StarryOS:ebpf-kmod` (`kernel/src/perf/kprobe.rs`).
//! Symbol resolution goes through the real in-kernel `.kallsyms` blob
//! (`crate::pseudofs::proc::KALLSYMS`), the same table `/proc/kallsyms` reads.

use alloc::{sync::Arc, vec::Vec};
use core::{
    any::Any,
    sync::atomic::{AtomicU32, Ordering},
};

use ax_errno::{AxError, AxResult};
use ax_memory_addr::PhysAddr;
use axpoll::Pollable;
use kbpf_basic::perf::{PerfProbeArgs, PerfProbeConfig};
use kprobe::{CallBackFunc, KretprobeBuilder, ProbeBuilder, PtRegs};

#[cfg(target_arch = "aarch64")]
use super::probe_sample;

/// Config value for entry probes (kprobe/uprobe), per Linux PERF_TYPE_PROBE ABI.
pub const PROBE_CONFIG_ENTRY: u64 = 0;
/// Config value for return probes (kretprobe/uretprobe), per Linux PERF_TYPE_PROBE ABI.
pub const PROBE_CONFIG_RETURN: u64 = 1;
/// Maximum number of concurrently active kretprobe instances, matching
/// Linux's `max(10, 2*NR_CPUS)` default for single-CPU configurations.
const KRETPROBE_MAX_ACTIVE: u32 = 10;

use crate::{
    file::FileLike,
    kprobe::{
        KernelKprobe, KernelKretprobe, KernelRawMutex, KprobeAuxiliary, register_kprobe,
        register_kretprobe, unregister_kprobe, unregister_kretprobe,
    },
    perf::{PerfEventOps, bpf::OwnedEbpfVm},
    uprobe::{KernelUprobe, unregister_uprobe},
};

/// One of {kprobe, kretprobe, uprobe}. Kprobe/kretprobe live in the global
/// kernel-text manager; uprobe lives in the firing process' per-process manager
/// (`ProcessData::uprobe_manager`), but exposes the same probe API.
#[derive(Debug)]
pub enum ProbeTy {
    Kprobe(Arc<KernelKprobe>),
    Kretprobe(Arc<KernelKretprobe>),
    Uprobe(Arc<KernelUprobe>),
}

/// Per-fd perf event wrapping a kprobe/kretprobe registration.
#[derive(Debug)]
pub struct ProbePerfEvent {
    _args: PerfProbeArgs,
    probe: ProbeTy,
    callback_list: Vec<u32>,
    /// Sampling state for `perf record -e kprobe:`/`uprobe:` (`sample_period > 0`):
    /// the mmap ring the hit-callback writes `PERF_RECORD_SAMPLE` into. `None` for
    /// a BPF-attach or non-sampling probe.
    #[cfg(target_arch = "aarch64")]
    sampling: Option<Arc<probe_sample::ProbeSampling>>,
}

impl ProbePerfEvent {
    /// Build a perf event tied to an already-registered probe.
    pub fn new(args: PerfProbeArgs, probe: ProbeTy) -> Self {
        Self {
            _args: args,
            probe,
            callback_list: Vec::new(),
            #[cfg(target_arch = "aarch64")]
            sampling: None,
        }
    }
}

/// Monotonic per-probe callback id, unique across the BPF and sample callbacks a
/// probe may carry. `Relaxed` suffices — only atomic unique-id allocation is
/// required, not synchronization with other memory.
fn next_callback_id() -> u32 {
    static CALLBACK_ID: AtomicU32 = AtomicU32::new(0);
    CALLBACK_ID.fetch_add(1, Ordering::Relaxed)
}

/// Register the sample-emitting callback (`perf record`) on `ev`'s probe when it
/// was opened with `sample_period > 0`. A sampling probe writes a
/// `PERF_RECORD_SAMPLE` into its ring on each hit (every `sample_period` hits).
#[cfg(target_arch = "aarch64")]
fn attach_sampling(
    ev: &mut ProbePerfEvent,
    sample_period: u64,
    sample_type: u64,
    is_user: bool,
    probe_addr: u64,
) -> AxResult<()> {
    if let Some(s) = probe_sample::make_sampling(sample_period, sample_type, is_user, probe_addr)? {
        let id = next_callback_id();
        let cb = probe_sample::ProbeSampleCallback::new(s.clone());
        match ev.probe {
            ProbeTy::Kprobe(ref k) => k.register_event_callback(id, cb),
            ProbeTy::Kretprobe(ref k) => k.register_event_callback(id, cb),
            ProbeTy::Uprobe(ref u) => u.register_event_callback(id, cb),
        }
        ev.callback_list.push(id);
        ev.sampling = Some(s);
    }
    Ok(())
}

/// Finish a probe open: attach the sample-emit callback (aarch64; a no-op off-arch
/// or for a non-sampling probe). Shared by the kprobe / uprobe opens.
#[cfg(target_arch = "aarch64")]
pub(crate) fn finish_probe_open(
    mut ev: ProbePerfEvent,
    sample_period: u64,
    sample_type: u64,
    is_user: bool,
    probe_addr: u64,
) -> AxResult<ProbePerfEvent> {
    attach_sampling(&mut ev, sample_period, sample_type, is_user, probe_addr)?;
    Ok(ev)
}

/// Off-arch stub: no sampling ring, so a probe stays BPF-attach-only.
#[cfg(not(target_arch = "aarch64"))]
pub(crate) fn finish_probe_open(
    ev: ProbePerfEvent,
    _sample_period: u64,
    _sample_type: u64,
    _is_user: bool,
    _probe_addr: u64,
) -> AxResult<ProbePerfEvent> {
    Ok(ev)
}

impl Drop for ProbePerfEvent {
    fn drop(&mut self) {
        for cid in &self.callback_list {
            match self.probe {
                ProbeTy::Kprobe(ref k) => k.unregister_event_callback(*cid),
                ProbeTy::Kretprobe(ref k) => k.unregister_event_callback(*cid),
                ProbeTy::Uprobe(ref u) => u.unregister_event_callback(*cid),
            }
        }
        match self.probe {
            ProbeTy::Kprobe(ref k) => unregister_kprobe(k.clone()),
            ProbeTy::Kretprobe(ref k) => unregister_kretprobe(k.clone()),
            ProbeTy::Uprobe(ref u) => unregister_uprobe(u.clone()),
        }
    }
}

impl Pollable for ProbePerfEvent {
    fn poll(&self) -> axpoll::IoEvents {
        // A sampling probe (`perf record`) is readable when its ring has unread
        // bytes. A BPF-attach probe delivers no fd readiness — its output goes
        // through the attached BPF program / ringbuf, a separate fd.
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
        let _ = (context, events);
    }
}

impl PerfEventOps for ProbePerfEvent {
    fn enable(&mut self) -> AxResult<()> {
        match self.probe {
            ProbeTy::Kprobe(ref k) => k.enable(),
            ProbeTy::Kretprobe(ref k) => k.kprobe().enable(),
            ProbeTy::Uprobe(ref u) => u.enable(),
        }
        Ok(())
    }

    fn disable(&mut self) -> AxResult<()> {
        match self.probe {
            ProbeTy::Kprobe(ref k) => k.disable(),
            ProbeTy::Kretprobe(ref k) => k.kprobe().disable(),
            ProbeTy::Uprobe(ref u) => u.disable(),
        }
        Ok(())
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    fn set_bpf_prog(&mut self, bpf_prog: Arc<dyn FileLike>) -> AxResult<()> {
        let vm = OwnedEbpfVm::new(bpf_prog)?;
        let id = next_callback_id();
        let callback = Arc::new(KprobePerfCallBack::new(vm));
        match self.probe {
            ProbeTy::Kprobe(ref k) => k.register_event_callback(id, callback),
            ProbeTy::Kretprobe(ref k) => k.register_event_callback(id, callback),
            ProbeTy::Uprobe(ref u) => u.register_event_callback(id, callback),
        }
        self.callback_list.push(id);
        Ok(())
    }

    /// `mmap(perf_fd)`: allocate the sampling ring for a `perf record` probe. For a
    /// BPF-attach or non-sampling probe (no ring), `mmap` stays unsupported.
    fn device_mmap(&mut self, len: usize) -> AxResult<(PhysAddr, Arc<dyn Any + Send + Sync>)> {
        #[cfg(target_arch = "aarch64")]
        if let Some(s) = &self.sampling {
            return s.device_mmap(len);
        }
        let _ = len;
        Err(AxError::Unsupported)
    }

    /// Record the event id a probe sample carries in `PERF_SAMPLE_ID` /
    /// `IDENTIFIER`. No-op for a non-sampling probe.
    fn set_sample_id(&mut self, id: u64) {
        #[cfg(target_arch = "aarch64")]
        if let Some(s) = &self.sampling {
            s.set_id(id);
            return;
        }
        let _ = id;
    }
}

/// Callback handed to the `kprobe` crate. When the probe fires, the crate
/// invokes `call(&mut pt_regs)` which we hand to the embedded rbpf VM as
/// its single-pointer context argument.
pub struct KprobePerfCallBack {
    /// `execute_with_ptregs` runs off `&self`, so the VM can be invoked
    /// directly from the immutable `call(&self, ..)` path — no interior
    /// mutability / lock required.
    vm: OwnedEbpfVm,
}

impl KprobePerfCallBack {
    fn new(vm: OwnedEbpfVm) -> Self {
        Self { vm }
    }
}

impl CallBackFunc for KprobePerfCallBack {
    fn call(&self, pt_regs: &mut PtRegs) {
        if let Err(e) = self.vm.execute_with_ptregs(pt_regs) {
            error!("kprobe BPF program failed: {e:?}");
        }
    }
}

fn lookup_symbol_addr(symbol: &str) -> AxResult<usize> {
    // Resolve against the real in-kernel `.kallsyms` blob (the same table
    // `/proc/kallsyms` is built from) rather than a separate stub.
    crate::pseudofs::proc::KALLSYMS
        .get()
        .and_then(|t| t.lookup_name(symbol))
        .map(|addr| addr as usize)
        .ok_or(AxError::NotFound)
}

fn perf_probe_arg_to_kprobe_builder(
    args: &PerfProbeArgs,
) -> AxResult<ProbeBuilder<KprobeAuxiliary>> {
    let symbol = &args.name;
    let addr = lookup_symbol_addr(symbol)?;
    Ok(ProbeBuilder::new()
        .with_symbol(symbol.clone())
        .with_symbol_addr(addr)
        .with_offset(0)
        .with_enable(false))
}

fn perf_probe_arg_to_kretprobe_builder(
    args: &PerfProbeArgs,
) -> AxResult<KretprobeBuilder<KernelRawMutex>> {
    let symbol = &args.name;
    let addr = lookup_symbol_addr(symbol)?;
    Ok(
        KretprobeBuilder::<KernelRawMutex>::new(KRETPROBE_MAX_ACTIVE)
            .with_symbol(symbol.clone())
            .with_symbol_addr(addr),
    )
}

/// Build a `ProbePerfEvent` for a `PERF_TYPE_KPROBE` perf_event_open call.
/// Config `PROBE_CONFIG_ENTRY` (0) = kprobe; `PROBE_CONFIG_RETURN` (1) = kretprobe.
///
/// When `sample_period > 0` (`perf record -e kprobe:`), a sample-emitting callback
/// is attached so each hit writes a `PERF_RECORD_SAMPLE` into the event's ring
/// (aarch64 only); otherwise the probe stays BPF-attach-only.
pub fn perf_event_open_kprobe(
    args: PerfProbeArgs,
    sample_period: u64,
    sample_type: u64,
) -> AxResult<ProbePerfEvent> {
    let probe = match args.config {
        PerfProbeConfig::Raw(PROBE_CONFIG_ENTRY) => {
            let builder = perf_probe_arg_to_kprobe_builder(&args)?;
            ProbeTy::Kprobe(register_kprobe(builder))
        }
        PerfProbeConfig::Raw(PROBE_CONFIG_RETURN) => {
            let builder = perf_probe_arg_to_kretprobe_builder(&args)?;
            ProbeTy::Kretprobe(register_kretprobe(builder))
        }
        _ => return Err(AxError::InvalidInput),
    };
    // The sample IP is the probe's (kallsyms) address, not the single-step pc.
    let probe_addr = lookup_symbol_addr(&args.name).unwrap_or(0) as u64;
    // Kprobe/kretprobe hits are kernel context (`is_user = false`).
    finish_probe_open(
        ProbePerfEvent::new(args, probe),
        sample_period,
        sample_type,
        false,
        probe_addr,
    )
}
