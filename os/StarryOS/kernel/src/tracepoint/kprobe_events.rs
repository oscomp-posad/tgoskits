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

use crate::pseudofs::{
    DirMaker, DirectRwFsFileOps, NodeOpsMux, SimpleDir, SimpleDirOps, SimpleFs, SpecialFsFile,
};

/// Base of the dynamic-kprobe-events id space. Static tracepoints are numbered
/// `0..N` by `global_init_events`; dynamic ids start well above that so a
/// `PERF_TYPE_TRACEPOINT` open can tell the two apart by id and route a dynamic
/// id to the kprobe path.
pub const KPROBE_EVENT_ID_BASE: u32 = 0x1000_0000;

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
    let mut map = KPROBE_EVENTS.lock();
    // Replace an existing (group,name) rather than duplicating it.
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

fn remove_event(group: &str, name: &str) -> bool {
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

/// `events/<group>/<event>/enable`. For `perf record` this is not the trigger
/// (perf uses `perf_event_open` + `ioctl(ENABLE)`); it exists so the tracefs
/// layout is complete and readable. v1 accepts `0`/`1` writes without arming a
/// standalone trace_pipe kprobe (that ftrace-style path is a later enhancement).
struct EventEnableFile;
impl DirectRwFsFileOps for EventEnableFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        read_from(b"0\n", buf, offset)
    }
    fn write_at(&self, buf: &[u8], _offset: u64) -> VfsResult<usize> {
        match core::str::from_utf8(buf).map(str::trim) {
            Ok("0") | Ok("1") => Ok(buf.len()),
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
            "enable" => {
                SpecialFsFile::new_regular_with_perm(self.fs.clone(), EventEnableFile, FILE_PERM)
                    .into()
            }
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
