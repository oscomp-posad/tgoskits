//! Dynamic kprobe events — the tracefs `kprobe_events` interface + the
//! `events/<group>/<event>/{id,format,enable}` tree the classic `perf` CLI uses.
//!
//! `perf probe -a func` writes `p:GROUP/EVENT SYMBOL` to
//! `/sys/kernel/debug/tracing/kprobe_events`; `perf record -e GROUP:EVENT` then
//! reads `events/GROUP/EVENT/id` and opens a `PERF_TYPE_TRACEPOINT` perf event on
//! that id. StarryOS's static tracepoint identity layer (the `.tracepoint` linker
//! section + a boot-frozen registry, an `events/` dir frozen once built, id/format
//! files bound to a `&'static TracePoint`) cannot represent a runtime-created
//! probe, so this module is a **parallel, runtime-mutable mirror** of just the
//! identity half: a lock-guarded registry with its own high id space, a live
//! tracefs directory (`SimpleDirOps` reading the registry), and per-event
//! id/format/enable files.
//!
//! The **firing** half is entirely reused: a dynamic id opened via
//! `perf_event_open(PERF_TYPE_TRACEPOINT)` is routed in [`crate::perf`] to the
//! kprobe path (`perf/kprobe.rs` `register_kprobe` + `perf/probe_sample.rs`
//! sample emission), because a kprobe fires through the breakpoint handler, not
//! the static-key tracepoint dispatch. The registry here holds only metadata
//! (symbol + id); the kprobe lifecycle lives in the opened `ProbePerfEvent`.

use alloc::{
    borrow::Cow,
    boxed::Box,
    collections::{BTreeMap, BTreeSet},
    format,
    string::{String, ToString},
    sync::Arc,
    vec::Vec,
};
use core::sync::atomic::{AtomicU32, Ordering};

use ax_kspin::SpinNoPreempt;
use axfs_ng_vfs::{NodePermission, VfsError, VfsResult};
use kprobe::{CallBackFunc, ProbeBuilder, PtRegs};
use ktracepoint::KernelTraceOps;

use super::KernelTraceAux;
use crate::{
    kprobe::{KernelKprobe, KprobeAuxiliary, register_kprobe, unregister_kprobe},
    pseudofs::{
        DirMaker, DirectRwFsFileOps, NodeOpsMux, SimpleDir, SimpleDirOps, SimpleFs, SpecialFsFile,
    },
};

/// Base of the dynamic-kprobe-events id space. Static tracepoints are numbered
/// `0..N` by `global_init_events` (N is the compile-time tracepoint count, at most
/// a few hundred); dynamic ids start well above that so a `PERF_TYPE_TRACEPOINT`
/// open can tell the two apart by id and route a dynamic id to the kprobe path.
///
/// The base **must fit `u16`**: a tracepoint record's `common_type` field is two
/// bytes, and `perf`/libtraceevent resolves a `PERF_SAMPLE_RAW` record to its
/// event by that field (`tep_find_event_by_record`). An id above `0xffff` would
/// truncate to a `common_type` that resolves to no event → a NULL deref in `perf
/// report`. `0x8000` clears any real static count while leaving 32 K dynamic ids.
pub const KPROBE_EVENT_ID_BASE: u32 = 0x8000;

/// Default group when `perf`/userspace omits one in `p:NAME sym` (Linux uses
/// `kprobes`). `perf probe` supplies its own group (`probe`).
const DEFAULT_GROUP: &str = "kprobes";

/// One dynamically-created kprobe event (metadata only; the kprobe itself is
/// placed by the opened `ProbePerfEvent`, not held here).
#[derive(Debug, Clone)]
struct DynKprobeEvent {
    id: u32,
    group: String,
    name: String,
    symbol: String,
    /// Byte offset past `symbol` for the probe address (`symbol+offset`). `perf`
    /// without a vmlinux expresses every kernel probe relative to `_stext`
    /// (e.g. `_stext+2404788`), so the offset is where the real target lives —
    /// dropping it would place the kprobe at the wrong address.
    offset: u64,
    /// `true` for a return probe (`r:`), `false` for an entry probe (`p:`).
    is_ret: bool,
}

/// The registry, keyed by id. A `SpinNoPreempt` (non-sleeping) lock: reads happen
/// from the tracefs file/dir ops (process context) and the perf-open routing.
static KPROBE_EVENTS: SpinNoPreempt<BTreeMap<u32, DynKprobeEvent>> =
    SpinNoPreempt::new(BTreeMap::new());
static NEXT_ID: AtomicU32 = AtomicU32::new(KPROBE_EVENT_ID_BASE);

/// Resolve a dynamic id to `(symbol, offset, is_ret)` for the perf-open routing.
/// `None` if `id` is not a dynamic kprobe event (a static tracepoint or unknown).
pub fn resolve(id: u32) -> Option<(String, u64, bool)> {
    if id < KPROBE_EVENT_ID_BASE {
        return None;
    }
    KPROBE_EVENTS
        .lock()
        .get(&id)
        .map(|e| (e.symbol.clone(), e.offset, e.is_ret))
}

/// Whether `symbol` exists in the kernel's kallsyms (same table `perf/kprobe.rs`
/// resolves against). A `perf probe` add on an unknown symbol must fail.
fn symbol_exists(symbol: &str) -> bool {
    crate::pseudofs::proc::KALLSYMS
        .get()
        .and_then(|t| t.lookup_name(symbol))
        .is_some()
}

/// Parse a number in decimal or `0x`-hex (perf writes decimal offsets).
fn parse_num(s: &str) -> Option<u64> {
    match s.strip_prefix("0x") {
        Some(hex) => u64::from_str_radix(hex, 16).ok(),
        None => s.parse().ok(),
    }
}

/// Parse a probe target `SYMBOL[+OFFS]` (ignoring any trailing `%return` / arg
/// spec) into `(symbol, offset)`. `perf` without a vmlinux writes every kernel
/// probe as `_stext+<offset>`, so the offset must be honored.
fn parse_symbol_offset(spec: &str) -> (String, u64) {
    // Strip a `%...`/` ...`-style suffix (arg specs); we keep only SYMBOL[+OFFS].
    let core = spec.split('%').next().unwrap_or(spec);
    match core.split_once('+') {
        Some((sym, off)) => (sym.to_string(), parse_num(off).unwrap_or(0)),
        None => (core.to_string(), 0),
    }
}

/// Parse + apply one `kprobe_events` write line. Accepts:
///   `p[:[GROUP/]NAME] SYMBOL[+OFFS]`   — add an entry probe
///   `r[:[GROUP/]NAME] SYMBOL`          — add a return probe
///   `-:[GROUP/]NAME`                   — remove an event
/// Arg-fetch specs beyond the symbol+offset are ignored (v1: no variable capture).
/// Returns `Err` (→ `EINVAL`) on malformed input / unknown symbol.
fn apply_line(line: &str) -> Result<(), &'static str> {
    let line = line.trim();
    if line.is_empty() {
        return Ok(());
    }
    let mut it = line.split_whitespace();
    let head = it.next().ok_or("empty")?;

    // `-:[GROUP/]NAME` removes.
    if let Some(spec) = head.strip_prefix("-:") {
        let (group, name) = split_group_name(spec);
        return if remove_event(&group, &name) {
            Ok(())
        } else {
            Err("no such event")
        };
    }

    let (kind, rest) = head.split_at(1);
    let is_ret = match kind {
        "p" => false,
        "r" => true,
        _ => return Err("probe type must be p or r"),
    };
    // rest is either "" or ":[GROUP/]NAME".
    let (group, name_opt) = match rest.strip_prefix(':') {
        Some(spec) => {
            let (g, n) = split_group_name(spec);
            (g, Some(n))
        }
        None if rest.is_empty() => (DEFAULT_GROUP.to_string(), None),
        None => return Err("malformed probe spec"),
    };

    let symbol_spec = it.next().ok_or("missing symbol")?;
    let (symbol, offset) = parse_symbol_offset(symbol_spec);
    if !symbol_exists(&symbol) {
        return Err("symbol not found in kallsyms");
    }
    // Default the event name to the symbol when userspace omits it.
    let name = name_opt.unwrap_or_else(|| symbol.clone());
    add_event(group, name, symbol, offset, is_ret);
    Ok(())
}

/// Split `GROUP/NAME` or bare `NAME` (→ default group).
fn split_group_name(spec: &str) -> (String, String) {
    match spec.split_once('/') {
        Some((g, n)) => (g.to_string(), n.to_string()),
        None => (DEFAULT_GROUP.to_string(), spec.to_string()),
    }
}

fn add_event(group: String, name: String, symbol: String, offset: u64, is_ret: bool) {
    // Disarm + drop any existing (group,name) rather than duplicating it (its id
    // is retired, so an armed ftrace probe under it must be torn down first).
    for id in ids_for(&group, &name) {
        disable_event(id);
    }
    let mut map = KPROBE_EVENTS.lock();
    map.retain(|_, e| !(e.group == group && e.name == name));
    let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    map.insert(
        id,
        DynKprobeEvent {
            id,
            group,
            name,
            symbol,
            offset,
            is_ret,
        },
    );
}

/// Ids currently registered for `(group, name)` (usually 0 or 1).
fn ids_for(group: &str, name: &str) -> Vec<u32> {
    KPROBE_EVENTS
        .lock()
        .values()
        .filter(|e| e.group == group && e.name == name)
        .map(|e| e.id)
        .collect()
}

fn remove_event(group: &str, name: &str) -> bool {
    // Disarm any live ftrace probe on this event before dropping its metadata.
    for id in ids_for(group, name) {
        disable_event(id);
    }
    let mut map = KPROBE_EVENTS.lock();
    let before = map.len();
    map.retain(|_, e| !(e.group == group && e.name == name));
    map.len() != before
}

/// The `kprobe_events` file's read content: one `p:GROUP/NAME SYMBOL[+OFFS]` line
/// each (the classic form perf/trace-cmd expect, offset shown when non-zero).
fn list_text() -> String {
    let map = KPROBE_EVENTS.lock();
    let mut out = String::new();
    for e in map.values() {
        let kind = if e.is_ret { 'r' } else { 'p' };
        if e.offset != 0 {
            out.push_str(&format!(
                "{}:{}/{} {}+{}\n",
                kind, e.group, e.name, e.symbol, e.offset
            ));
        } else {
            out.push_str(&format!("{}:{}/{} {}\n", kind, e.group, e.name, e.symbol));
        }
    }
    out
}

fn groups() -> Vec<String> {
    let map = KPROBE_EVENTS.lock();
    map.values()
        .map(|e| e.group.clone())
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect()
}

fn events_in_group(group: &str) -> Vec<String> {
    let map = KPROBE_EVENTS.lock();
    map.values()
        .filter(|e| e.group == group)
        .map(|e| e.name.clone())
        .collect()
}

fn find(group: &str, name: &str) -> Option<DynKprobeEvent> {
    KPROBE_EVENTS
        .lock()
        .values()
        .find(|e| e.group == group && e.name == name)
        .cloned()
}

fn event_by_id(id: u32) -> Option<DynKprobeEvent> {
    KPROBE_EVENTS.lock().get(&id).cloned()
}

/// Event name for a dynamic id — used by [`super::KernelTraceAux::dynamic_event`]
/// to render a `trace`/`trace_pipe` line for a kprobe hit.
pub fn event_name_by_id(id: u32) -> Option<String> {
    KPROBE_EVENTS.lock().get(&id).map(|e| e.name.clone())
}

// ---------------------------------------------------------------------------
// ftrace `enable`-file path: `echo 1 > events/<grp>/<evt>/enable` arms the kprobe
// so hits land in the trace ring (`cat trace` / `trace_pipe`), independent of
// `perf_event_open`. Mirrors Linux's classic kprobe-events flow.
// ---------------------------------------------------------------------------

/// Bytes of one ftrace kprobe record: the 8-byte common header
/// (`common_type`,`common_flags`,`common_preempt_count`,`common_pid`) + the
/// 8-byte `__probe_ip`. Matches [`EventFormatFile`] and `ktracepoint`'s
/// `TraceEntry` layout.
const TRACE_RECORD_LEN: usize = 16;

/// An armed ftrace kprobe: the registration + the callback id, so `echo 0`
/// (or a re-add) can detach it.
struct EnabledProbe {
    kprobe: alloc::sync::Arc<KernelKprobe>,
    callback_id: u32,
}

impl core::fmt::Debug for EnabledProbe {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("EnabledProbe")
            .field("callback_id", &self.callback_id)
            .finish()
    }
}

/// Event id → its armed ftrace probe. Present iff the event's `enable` is `1`.
static ENABLED: SpinNoPreempt<BTreeMap<u32, EnabledProbe>> = SpinNoPreempt::new(BTreeMap::new());
static NEXT_CB_ID: AtomicU32 = AtomicU32::new(1);

/// The per-hit callback: writes one ftrace record (common header + `__probe_ip`)
/// into the trace ring. Runs in the kprobe's synchronous break-exception context
/// on the interrupted thread — non-sleeping, like the static tracepoint fire path.
struct TraceKprobeCallback {
    common_type: u16,
    probe_addr: u64,
}

impl CallBackFunc for TraceKprobeCallback {
    fn call(&self, _pt_regs: &mut PtRegs) {
        let pid = KernelTraceAux::current_pid();
        let mut buf = [0u8; TRACE_RECORD_LEN];
        buf[0..2].copy_from_slice(&self.common_type.to_ne_bytes()); // common_type
        // common_flags (2) + common_preempt_count (3) stay 0.
        buf[4..8].copy_from_slice(&(pid as i32).to_ne_bytes()); // common_pid
        buf[8..16].copy_from_slice(&self.probe_addr.to_ne_bytes()); // __probe_ip
        // Cache the process name first so the reader can label the record, then
        // push (the reader renders the event via `KernelTraceAux::dynamic_event`).
        KernelTraceAux::trace_cmdline_push(pid);
        KernelTraceAux::trace_pipe_push_raw_record(&buf);
    }
}

/// Resolve `symbol` to its kallsyms address (same table `perf/kprobe.rs` uses).
fn symbol_addr(symbol: &str) -> Option<usize> {
    crate::pseudofs::proc::KALLSYMS
        .get()
        .and_then(|t| t.lookup_name(symbol))
        .map(|a| a as usize)
}

/// Arm the event's kprobe so hits write into the trace ring. Idempotent.
fn enable_event(id: u32) -> Result<(), &'static str> {
    if ENABLED.lock().contains_key(&id) {
        return Ok(());
    }
    let ev = event_by_id(id).ok_or("no such event")?;
    // v1: entry probes only in the ftrace path (return probes still work via
    // perf_event_open); a return probe's `enable` is a no-op success.
    if ev.is_ret {
        return Ok(());
    }
    let addr = symbol_addr(&ev.symbol).ok_or("symbol not found")?;
    let probe_addr = addr as u64 + ev.offset;
    let builder = ProbeBuilder::<KprobeAuxiliary>::new()
        .with_symbol(ev.symbol.clone())
        .with_symbol_addr(addr)
        .with_offset(ev.offset as usize)
        .with_enable(true);
    let kprobe = register_kprobe(builder);
    let callback_id = NEXT_CB_ID.fetch_add(1, Ordering::Relaxed);
    kprobe.register_event_callback(
        callback_id,
        alloc::sync::Arc::new(TraceKprobeCallback {
            common_type: id as u16,
            probe_addr,
        }),
    );
    ENABLED.lock().insert(
        id,
        EnabledProbe {
            kprobe,
            callback_id,
        },
    );
    Ok(())
}

/// Disarm the event's ftrace kprobe (detach the callback + unregister). Idempotent.
fn disable_event(id: u32) {
    let Some(entry) = ENABLED.lock().remove(&id) else {
        return;
    };
    entry.kprobe.unregister_event_callback(entry.callback_id);
    unregister_kprobe(entry.kprobe);
}

fn is_enabled(id: u32) -> bool {
    ENABLED.lock().contains_key(&id)
}

// ---------------------------------------------------------------------------
// tracefs files
// ---------------------------------------------------------------------------

/// Helper: serve `content` from a `read_at(buf, offset)` (the common pattern for
/// these small text files).
fn read_from(content: &[u8], buf: &mut [u8], offset: u64) -> VfsResult<usize> {
    let offset = offset as usize;
    if offset >= content.len() {
        return Ok(0);
    }
    let n = buf.len().min(content.len() - offset);
    buf[..n].copy_from_slice(&content[offset..offset + n]);
    Ok(n)
}

/// `/sys/kernel/debug/tracing/kprobe_events`.
pub struct KprobeEventsFile;

impl DirectRwFsFileOps for KprobeEventsFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        read_from(list_text().as_bytes(), buf, offset)
    }

    fn write_at(&self, buf: &[u8], _offset: u64) -> VfsResult<usize> {
        let text = core::str::from_utf8(buf).map_err(|_| VfsError::InvalidInput)?;
        // A single write may carry several newline-separated commands.
        for line in text.split(['\n', ';']) {
            apply_line(line).map_err(|_| VfsError::InvalidInput)?;
        }
        Ok(buf.len())
    }
}

/// `events/<group>/<event>/id`.
struct EventIdFile {
    id: u32,
}
impl DirectRwFsFileOps for EventIdFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        read_from(format!("{}\n", self.id).as_bytes(), buf, offset)
    }
}

/// `events/<group>/<event>/format` — a minimal but libtraceevent-parseable
/// tracepoint format: the four `common_*` header fields plus a synthetic
/// `__probe_ip` (what a Linux kprobe event with no args exposes).
struct EventFormatFile {
    id: u32,
    name: String,
}
impl DirectRwFsFileOps for EventFormatFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        let text = format!(
            "name: {}\nID: {}\nformat:\n\tfield:unsigned short \
             common_type;\toffset:0;\tsize:2;\tsigned:0;\n\tfield:unsigned char \
             common_flags;\toffset:2;\tsize:1;\tsigned:0;\n\tfield:unsigned char \
             common_preempt_count;\toffset:3;\tsize:1;\tsigned:0;\n\tfield:int \
             common_pid;\toffset:4;\tsize:4;\tsigned:1;\n\n\tfield:unsigned long \
             __probe_ip;\toffset:8;\tsize:8;\tsigned:0;\n\nprint fmt: \"(%lx)\", REC->__probe_ip\n",
            self.name, self.id,
        );
        read_from(text.as_bytes(), buf, offset)
    }
}

/// `events/<group>/<event>/enable`. `echo 1` arms the event's kprobe so hits are
/// written into the trace ring (`cat trace` / `trace_pipe`) — the classic ftrace
/// kprobe flow, independent of `perf record` (which drives the probe through
/// `perf_event_open` + `ioctl(ENABLE)` instead). `echo 0` disarms it; the read
/// reflects the live armed state.
struct EventEnableFile {
    id: u32,
}
impl DirectRwFsFileOps for EventEnableFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        let content: &[u8] = if is_enabled(self.id) { b"1\n" } else { b"0\n" };
        read_from(content, buf, offset)
    }
    fn write_at(&self, buf: &[u8], _offset: u64) -> VfsResult<usize> {
        match core::str::from_utf8(buf).map(str::trim) {
            Ok("1") => {
                enable_event(self.id).map_err(|_| VfsError::InvalidInput)?;
                Ok(buf.len())
            }
            Ok("0") => {
                disable_event(self.id);
                Ok(buf.len())
            }
            _ => Err(VfsError::InvalidInput),
        }
    }
}

// ---------------------------------------------------------------------------
// live tracefs directories (read the registry on each lookup)
// ---------------------------------------------------------------------------

const FILE_PERM: NodePermission = NodePermission::from_bits_truncate(0o644);

/// `events/<group>/<event>/` — the three attribute files for one dynamic event.
struct EventDir {
    fs: Arc<SimpleFs>,
    group: String,
    name: String,
}
impl SimpleDirOps for EventDir {
    fn child_names<'a>(&'a self) -> Box<dyn Iterator<Item = Cow<'a, str>> + 'a> {
        Box::new(["id", "format", "enable"].into_iter().map(Cow::Borrowed))
    }

    fn lookup_child(&self, name: &str) -> VfsResult<NodeOpsMux> {
        let ev = find(&self.group, &self.name).ok_or(VfsError::NotFound)?;
        let file: NodeOpsMux = match name {
            "id" => SpecialFsFile::new_regular_with_perm(
                self.fs.clone(),
                EventIdFile { id: ev.id },
                FILE_PERM,
            )
            .into(),
            "format" => SpecialFsFile::new_regular_with_perm(
                self.fs.clone(),
                EventFormatFile {
                    id: ev.id,
                    name: ev.name.clone(),
                },
                FILE_PERM,
            )
            .into(),
            "enable" => SpecialFsFile::new_regular_with_perm(
                self.fs.clone(),
                EventEnableFile { id: ev.id },
                FILE_PERM,
            )
            .into(),
            _ => return Err(VfsError::NotFound),
        };
        Ok(file)
    }

    fn is_cacheable(&self) -> bool {
        false
    }
}

/// `events/<group>/` — one subdir per dynamic event in the group.
struct GroupDir {
    fs: Arc<SimpleFs>,
    group: String,
}
impl SimpleDirOps for GroupDir {
    fn child_names<'a>(&'a self) -> Box<dyn Iterator<Item = Cow<'a, str>> + 'a> {
        Box::new(events_in_group(&self.group).into_iter().map(Cow::Owned))
    }

    fn lookup_child(&self, name: &str) -> VfsResult<NodeOpsMux> {
        if find(&self.group, name).is_none() {
            return Err(VfsError::NotFound);
        }
        let maker: DirMaker = SimpleDir::new_maker(
            self.fs.clone(),
            Arc::new(EventDir {
                fs: self.fs.clone(),
                group: self.group.clone(),
                name: name.to_string(),
            }),
        );
        Ok(maker.into())
    }

    fn is_cacheable(&self) -> bool {
        false
    }
}

/// The dynamic slice of `events/` — the groups holding dynamic kprobe events.
/// Chained after the static subsystem `DirMapping` in [`super::init_events`], so
/// `events/` shows both static tracepoints and runtime `perf probe` groups.
pub struct DynamicEventsDir {
    fs: Arc<SimpleFs>,
}

impl DynamicEventsDir {
    pub fn new(fs: Arc<SimpleFs>) -> Self {
        Self { fs }
    }
}

impl SimpleDirOps for DynamicEventsDir {
    fn child_names<'a>(&'a self) -> Box<dyn Iterator<Item = Cow<'a, str>> + 'a> {
        Box::new(groups().into_iter().map(Cow::Owned))
    }

    fn lookup_child(&self, name: &str) -> VfsResult<NodeOpsMux> {
        if !groups().iter().any(|g| g == name) {
            return Err(VfsError::NotFound);
        }
        let maker: DirMaker = SimpleDir::new_maker(
            self.fs.clone(),
            Arc::new(GroupDir {
                fs: self.fs.clone(),
                group: name.to_string(),
            }),
        );
        Ok(maker.into())
    }

    fn is_cacheable(&self) -> bool {
        false
    }
}
