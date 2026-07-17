//! See Linux Documentation for details: <https://docs.kernel.org/trace/ftrace.html>
mod control;
pub mod kprobe_events;
mod sched;
mod trace;
mod trace_pipe;

use alloc::{collections::BTreeMap, string::ToString, sync::Arc, vec::Vec};
use core::{
    num::NonZero,
    ops::Deref,
    sync::atomic::{AtomicBool, Ordering},
};

use ax_errno::{AxError, AxResult};
use ax_kspin::SpinNoPreempt;
use ax_lazyinit::LazyInit;
use ax_memory_addr::VirtAddr;
use ax_runtime::hal::{percpu::this_cpu_id, time::monotonic_time_nanos};
use ax_task::{IrqNotify, current};
use axfs_ng_vfs::NodePermission;
use axpoll::{IoEvents, PollSet};
use ktracepoint::*;

use crate::{
    pseudofs::{
        DirMaker, DirMapping, NodeOpsMux, SeqObject, SimpleDir, SimpleDirOps, SimpleFs,
        SpecialFsFile,
    },
    task::AsThread,
};

/// Maximum number of trace records kept in the raw trace pipe ring buffer.
const TRACE_RAW_PIPE_CAPACITY: usize = 4096;
/// Maximum number of PID→cmdline entries in the command-line cache.
const TRACE_CMDLINE_CACHE_SIZE: usize = 4096;

// The registry entry is locked from the tracepoint fire path, which for
// `sched:sched_switch` runs inside `axtask::switch_to` (IRQ off,
// preemption disabled). A sleeping `ax_sync::Mutex` would trip the
// "sleeping in atomic context" guard there, so this lock must be a
// non-sleeping spinlock — the same kind the perf output path (`PERF_FILE`)
// uses for exactly this reason.
pub type KernelExtTracePoint = Arc<SpinNoPreempt<ExtTracePoint<KernelTraceAux>>>;

/// Look up a registered tracepoint by its numeric id (as found in
/// `/sys/kernel/debug/tracing/events/<subsystem>/<event>/id`).
///
/// Returns `None` if the id is unknown or the registry has not been
/// initialized yet.
pub fn lookup_ext_tracepoint(id: u32) -> Option<KernelExtTracePoint> {
    TRACE_STATE.ext_tracepoints.get()?.get(&id).cloned()
}

/// Find a registered tracepoint by name. Returns the first `ExtTracePoint`
/// whose underlying `TracePoint`'s name matches `name`.
///
/// Returns `None` if no tracepoint matches or the registry has not been
/// initialized yet.
pub fn find_ext_tracepoint_by_name(name: &str) -> Option<KernelExtTracePoint> {
    for ext_tp in TRACE_STATE.ext_tracepoints.get()?.values() {
        if ext_tp.lock().trace_point().name() == name {
            return Some(ext_tp.clone());
        }
    }
    None
}

struct TraceState {
    point_map: LazyInit<TracePointMap<KernelTraceAux>>,
    // The trace ring buffer and the pid→cmdline cache are BOTH written from the
    // tracepoint fire path (`trace_pipe_push_raw_record` / `trace_cmdline_push`),
    // which runs with preemption disabled (the registry entry above is
    // `SpinNoPreempt`; for `sched:sched_switch` the fire path is inside
    // `axtask::switch_to`, IRQs off). A sleeping `ax_sync::Mutex` here would try to
    // sleep in atomic context on the first enabled-tracepoint hit and wedge the
    // CPU, so — like the registry entry and the perf output path — these are
    // non-sleeping spinlocks. Every current tracepoint fires from a preempt-gated
    // context (syscall path or `switch_to`), never a hard-IRQ handler, so
    // `SpinNoPreempt` suffices; a tracepoint added to hard-IRQ context would need
    // `SpinNoIrq` here (and for the registry entry).
    raw_pipe: SpinNoPreempt<TracePipeRaw>,
    pipe_event: PollSet,
    pipe_notify: IrqNotify,
    cmdline_cache: LazyInit<SpinNoPreempt<TraceCmdLineCache>>,
    ext_tracepoints: LazyInit<BTreeMap<u32, KernelExtTracePoint>>,
}

impl TraceState {
    const fn new() -> Self {
        Self {
            point_map: LazyInit::new(),
            raw_pipe: SpinNoPreempt::new(TracePipeRaw::new(TRACE_RAW_PIPE_CAPACITY)),
            pipe_event: PollSet::new(),
            pipe_notify: IrqNotify::new(),
            cmdline_cache: LazyInit::new(),
            ext_tracepoints: LazyInit::new(),
        }
    }
}

static TRACE_STATE: TraceState = TraceState::new();
static TRACE_PIPE_NOTIFY_WORKER: AtomicBool = AtomicBool::new(false);

pub struct KernelTraceAux;

impl KernelTraceOps for KernelTraceAux {
    fn current_pid() -> u32 {
        let curr = current();
        let proc_data = &curr.as_thread().proc_data;
        proc_data.proc.pid()
    }

    fn trace_pipe_push_raw_record(buf: &[u8]) {
        // Build the owned record BEFORE taking the ring lock so the heap copy is
        // not done inside the (preempt-disabled) critical section — keep the lock
        // hold to the bounded ring append. See `TraceState::raw_pipe`.
        let timestamp = monotonic_time_nanos();
        let cpu_id = this_cpu_id() as _;
        let event = buf.to_vec();
        TRACE_STATE
            .raw_pipe
            .lock()
            .push_record(timestamp, cpu_id, event);
        TRACE_STATE.pipe_notify.notify_irq();
    }

    fn trace_cmdline_push(pid: u32) {
        let curr = current();
        let proc_data = &curr.as_thread().proc_data;
        let exe_path = proc_data.exe_path.read();
        let pname = exe_path
            .split(' ')
            .next()
            .unwrap_or("unknown")
            .split('/')
            .next_back()
            .unwrap_or("unknown");
        TRACE_STATE.cmdline_cache.lock().insert(pid, pname);
    }

    fn write_kernel_text(addr: *mut core::ffi::c_void, data: &[u8]) {
        crate::mm::write_kernel_text(VirtAddr::from_mut_ptr_of(addr), data)
            .expect("Failed to write kernel text");
    }

    fn read_tracepoint_state<R>(id: u32, f: impl FnOnce(&ExtTracePoint<Self>) -> R) -> R {
        let ext_tp = TRACE_STATE
            .ext_tracepoints
            .deref()
            .get(&id)
            .expect("Tracepoint not found");
        f(ext_tp.lock().deref())
    }

    fn write_tracepoint_state<R>(id: u32, f: impl FnOnce(&mut ExtTracePoint<Self>) -> R) -> R {
        let ext_tp = TRACE_STATE
            .ext_tracepoints
            .deref()
            .get(&id)
            .expect("Tracepoint not found");
        let mut ext_tp = ext_tp.lock();
        f(&mut ext_tp)
    }
}

fn start_trace_pipe_notify_worker() {
    if TRACE_PIPE_NOTIFY_WORKER.swap(true, Ordering::AcqRel) {
        return;
    }
    ax_task::spawn_with_name(
        || loop {
            TRACE_STATE.pipe_notify.wait();
            // Trace records are queued before the deferred poll wake.
            unsafe { TRACE_STATE.pipe_event.wake(IoEvents::IN) };
        },
        "trace-pipe-notify".into(),
    );
}

/// Carries the unread suffix of a formatted text record across `read_at` calls.
///
/// Tracefs text records are consumed as whole records from the backing trace
/// buffer, but the user-provided read buffer may be smaller than one formatted
/// line. This helper lets callers return the prefix immediately and keep the
/// suffix for later reads, avoiding a false EOF when `buf` is too small.
struct TextDrain {
    pending: Vec<u8>,
    pos: usize,
}

impl TextDrain {
    /// Creates an empty text drain with no pending bytes.
    const fn new() -> Self {
        Self {
            pending: Vec::new(),
            pos: 0,
        }
    }

    /// Discards any pending bytes and returns the drain to the initial state.
    fn reset(&mut self) {
        self.pending.clear();
        self.pos = 0;
    }

    /// Copies as many pending bytes as possible into `buf`.
    ///
    /// Returns the number of bytes copied. If all pending bytes are drained,
    /// the internal state is reset so the next read can consume a new record.
    fn drain_pending(&mut self, buf: &mut [u8]) -> usize {
        if self.pending.is_empty() {
            return 0;
        }

        let remaining = &self.pending[self.pos..];
        let len = remaining.len().min(buf.len());
        buf[..len].copy_from_slice(&remaining[..len]);
        self.pos += len;

        if self.pos == self.pending.len() {
            self.reset();
        }
        len
    }

    /// Copies one formatted record into `buf` starting at `copy_len`.
    ///
    /// Returns `false` when `buf` has no remaining space and the caller should
    /// stop without consuming a new backing record. If only a prefix fits, the
    /// remaining suffix is stored internally and the method returns `true`, so
    /// the caller may consume the backing record.
    fn copy_record(&mut self, record: &[u8], buf: &mut [u8], copy_len: &mut usize) -> bool {
        if record.is_empty() {
            return true;
        }

        let remaining = buf.len() - *copy_len;
        if remaining == 0 {
            return false;
        }

        let len = record.len().min(remaining);
        buf[*copy_len..*copy_len + len].copy_from_slice(&record[..len]);
        *copy_len += len;

        if len < record.len() {
            self.pending.extend_from_slice(&record[len..]);
        }
        true
    }
}

fn common_trace_pipe_read(
    trace_buf: &mut dyn TracePipeOps,
    drain: &mut TextDrain,
    buf: &mut [u8],
) -> usize {
    let mut copy_len = drain.drain_pending(buf);
    if copy_len == buf.len() {
        return copy_len;
    }

    let trace_cmdline_cache = TRACE_STATE.cmdline_cache.lock();
    loop {
        if let Some(record) = trace_buf.peek() {
            let record_str = TraceEntryParser::parse::<KernelTraceAux>(
                &TRACE_STATE.point_map,
                &trace_cmdline_cache,
                record,
            );
            if !drain.copy_record(record_str.as_bytes(), buf, &mut copy_len) {
                break;
            }
            trace_buf.pop(); // Remove the record after reading

            if copy_len == buf.len() {
                break;
            }
            continue;
        }
        break;
    }
    copy_len
}

/// Initialize registered tracepoints. This should be called after static keys are initialized, and before any tracepoint is hit.
pub fn tracepoint_init() -> AxResult<()> {
    let (tp_map, ext_tps) =
        global_init_events::<KernelTraceAux>().map_err(|_| AxError::InvalidInput)?;

    let ext_tps = ext_tps
        .into_iter()
        .map(|ext_tp| (ext_tp.id(), Arc::new(SpinNoPreempt::new(ext_tp))))
        .collect::<BTreeMap<_, _>>();

    ax_println!("Initialized {} tracepoints", tp_map.len());
    TRACE_STATE.point_map.init_once(tp_map);
    TRACE_STATE.ext_tracepoints.init_once(ext_tps);
    TRACE_STATE
        .cmdline_cache
        .init_once(SpinNoPreempt::new(TraceCmdLineCache::new(
            NonZero::new(TRACE_CMDLINE_CACHE_SIZE).unwrap(),
        )));
    start_trace_pipe_notify_worker();
    Ok(())
}

/// Initialize events directory in debugfs
/// `events/header_page` — the trace ring-buffer page header layout. `perf record`
/// on a tracepoint/probe event embeds this (with `header_event` and each event's
/// `format`) into perf.data's `HEADER_TRACING_DATA`; libtraceevent then uses it to
/// decode every sample's `PERF_SAMPLE_RAW` payload. Without it `perf report` aborts
/// with "broken or missing trace data". Standard Linux content; 4 KiB page →
/// 4096 − 16 = 4080 data bytes.
const TRACE_HEADER_PAGE: &str = "\tfield: u64 timestamp;\toffset:0;\tsize:8;\tsigned:0;\n\tfield: \
                                 local_t commit;\toffset:8;\tsize:8;\tsigned:1;\n\tfield: int \
                                 overwrite;\toffset:8;\tsize:1;\tsigned:1;\n\tfield: char \
                                 data;\toffset:16;\tsize:4080;\tsigned:1;\n";

/// `events/header_event` — the compressed ring-buffer record header
/// libtraceevent expects alongside `header_page`. Standard Linux content.
const TRACE_HEADER_EVENT: &str = "# compressed entry header\n\ttype_len    :    5 \
                                  bits\n\ttime_delta  :   27 bits\n\tarray       :   32 \
                                  bits\n\n\tpadding     : type == 29\n\ttime_extend : type == \
                                  30\n\ttime_stamp  : type == 31\n\tdata max type_len  == 28\n";

/// `events/ftrace/print/format` — the standard `ftrace:print` event. `perf
/// record`'s tracing-data packer *unconditionally* reads the `ftrace` subsystem
/// (`copy_event_system("events/ftrace")`); a missing directory makes perf drop
/// the whole `TRACING_DATA` feature, so `perf report` then fails with "broken or
/// missing trace data". Providing this one standard event satisfies the packer.
const FTRACE_PRINT_FORMAT: &str = "name: print\nID: 5\nformat:\n\tfield:unsigned short \
                                   common_type;\toffset:0;\tsize:2;\tsigned:0;\n\tfield:unsigned \
                                   char common_flags;\toffset:2;\tsize:1;\tsigned:0;\n\tfield:\
                                   unsigned char common_preempt_count;\toffset:3;\tsize:1;\t\
                                   signed:0;\n\tfield:int common_pid;\toffset:4;\tsize:4;\t\
                                   signed:1;\n\n\tfield:unsigned long ip;\toffset:8;\tsize:8;\t\
                                   signed:0;\n\tfield:char buf[];\toffset:16;\tsize:0;\tsigned:0;\
                                   \n\nprint fmt: \"%ps: %s\", (void *)REC->ip, REC->buf\n";

/// Build a read-only tracefs file serving fixed `content` (for `DirMapping::add`).
fn static_text_file(fs: &Arc<SimpleFs>, content: &'static str) -> NodeOpsMux {
    SpecialFsFile::new_regular_with_perm(
        fs.clone(),
        SeqObject::new(move || Ok(content.to_string())),
        NodePermission::from_bits_truncate(0o440),
    )
    .into()
}

fn init_events(fs: Arc<SimpleFs>) -> DirMaker {
    let mut events_root = DirMapping::new();
    let mut subsystem = BTreeMap::new();

    for ext_tp in TRACE_STATE.ext_tracepoints.deref().values() {
        let tp = ext_tp.lock().trace_point();
        let subsystem_name = tp.system();
        let event_name = tp.name();

        let subsystem_root = {
            if !subsystem.contains_key(subsystem_name) {
                let new_root = DirMapping::new();
                subsystem.insert(subsystem_name.to_string(), new_root);
            }
            subsystem.get_mut(subsystem_name).unwrap()
        };

        let mut event_root = DirMapping::new();
        event_root.add(
            "enable",
            SpecialFsFile::new_regular_with_perm(
                fs.clone(),
                control::EventEnableObj::new(ext_tp.clone()),
                NodePermission::from_bits_truncate(0o640),
            ),
        );
        event_root.add("format", {
            let seq_obj = SeqObject::new({
                let format_file = TracePointFormatFile::new(tp);
                move || Ok(format_file.read())
            });
            SpecialFsFile::new_regular_with_perm(
                fs.clone(),
                seq_obj,
                NodePermission::from_bits_truncate(0o440),
            )
        });

        event_root.add("id", {
            let seq_obj = SeqObject::new({
                let id_file = TracePointIdFile::new(tp);
                move || Ok(id_file.read())
            });
            SpecialFsFile::new_regular_with_perm(
                fs.clone(),
                seq_obj,
                NodePermission::from_bits_truncate(0o440),
            )
        });
        event_root.add(
            "filter",
            SpecialFsFile::new_regular_with_perm(
                fs.clone(),
                control::EventFilterObj::new(ext_tp.clone()),
                NodePermission::from_bits_truncate(0o640),
            ),
        );
        subsystem_root.add(
            event_name,
            SimpleDir::new_maker(fs.clone(), Arc::new(event_root)),
        );
    }
    for (subsystem_name, subsystem_root) in subsystem {
        events_root.add(
            &subsystem_name,
            SimpleDir::new_maker(fs.clone(), Arc::new(subsystem_root)),
        );
    }
    // `perf record` reads these two files (plus each event's `format`) to build
    // perf.data's tracing-data section; without them `perf report` fails with
    // "broken or missing trace data".
    events_root.add("header_page", static_text_file(&fs, TRACE_HEADER_PAGE));
    events_root.add("header_event", static_text_file(&fs, TRACE_HEADER_EVENT));
    // perf's tracing-data packer always reads the `ftrace` subsystem; a missing
    // dir drops the whole TRACING_DATA feature. Provide the standard `print` event.
    {
        let mut ftrace_sys = DirMapping::new();
        let mut print_evt = DirMapping::new();
        print_evt.add("format", static_text_file(&fs, FTRACE_PRINT_FORMAT));
        print_evt.add("id", static_text_file(&fs, "5\n"));
        print_evt.add("enable", static_text_file(&fs, "0\n"));
        ftrace_sys.add(
            "print",
            SimpleDir::new_maker(fs.clone(), Arc::new(print_evt)),
        );
        events_root.add(
            "ftrace",
            SimpleDir::new_maker(fs.clone(), Arc::new(ftrace_sys)),
        );
    }
    // Chain a live, runtime-mutable slice after the static subsystems so
    // `perf probe`'s dynamic `events/<group>/` groups appear alongside the
    // compile-time tracepoints (the chained ops report non-cacheable, so the VFS
    // re-queries and picks up events added after boot).
    let dynamic = kprobe_events::DynamicEventsDir::new(fs.clone());
    SimpleDir::new_maker(fs, Arc::new(events_root.chain(dynamic)))
}

/// Initialize tracing directory in debugfs
pub fn init_tracing_dir(fs: Arc<SimpleFs>) -> DirMaker {
    let mut tracing_root = DirMapping::new();
    tracing_root.set_cacheable(false);

    // `perf probe -a func` writes `p:GROUP/EVENT SYMBOL` here to create a dynamic
    // kprobe event; the resulting `events/GROUP/EVENT/` is served by the chained
    // dynamic dir in `init_events`.
    tracing_root.add(
        "kprobe_events",
        SpecialFsFile::new_regular_with_perm(
            fs.clone(),
            kprobe_events::KprobeEventsFile,
            NodePermission::from_bits_truncate(0o644),
        ),
    );

    tracing_root.add(
        "saved_cmdlines_size",
        SpecialFsFile::new_regular_with_perm(
            fs.clone(),
            control::TraceCmdLineSizeObj,
            NodePermission::from_bits_truncate(0o640),
        ),
    );
    tracing_root.add(
        "trace_pipe",
        SpecialFsFile::new_regular_with_perm(
            fs.clone(),
            trace_pipe::TracePipeFile::new(),
            NodePermission::from_bits_truncate(0o440),
        ),
    );
    tracing_root.add_dynamic("saved_cmdlines", {
        let fs = fs.clone();
        move || {
            SpecialFsFile::new_regular_with_perm(
                fs.clone(),
                trace::TraceCmdLineFile::new(),
                NodePermission::from_bits_truncate(0o440),
            )
            .into()
        }
    });
    tracing_root.add_dynamic("trace", {
        let fs = fs.clone();
        move || {
            SpecialFsFile::new_regular_with_perm(
                fs.clone(),
                trace::TraceFile::new(),
                NodePermission::from_bits_truncate(0o640),
            )
            .into()
        }
    });
    tracing_root.add("events", init_events(fs.clone()));
    SimpleDir::new_maker(fs, Arc::new(tracing_root))
}
