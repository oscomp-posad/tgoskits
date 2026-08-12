use alloc::{
    format,
    string::{String, ToString},
    vec::Vec,
};
use core::num::NonZero;

use lru::LruCache;

use crate::{KernelTraceOps, TraceEntry, TracePointMap};

/// A trace pipe record with host-provided metadata captured when the event is written.
#[derive(Clone, Debug)]
pub struct TracePipeRecord {
    timestamp: u64,
    cpu_id: u32,
    event: Vec<u8>,
}

impl TracePipeRecord {
    /// Create a new trace pipe record.
    pub fn new(timestamp: u64, cpu_id: u32, event: Vec<u8>) -> Self {
        Self {
            timestamp,
            cpu_id,
            event,
        }
    }

    /// Overwrite this record in place, reusing the payload buffer's capacity (no
    /// allocation when `data` fits). Used by the alloc-free circular push.
    pub fn set(&mut self, timestamp: u64, cpu_id: u32, data: &[u8]) {
        self.timestamp = timestamp;
        self.cpu_id = cpu_id;
        self.event.clear();
        self.event.extend_from_slice(data);
    }

    /// The event timestamp in nanoseconds.
    pub fn timestamp(&self) -> u64 {
        self.timestamp
    }

    /// The CPU ID on which the event was recorded.
    pub fn cpu_id(&self) -> u32 {
        self.cpu_id
    }

    /// The raw trace event payload.
    pub fn event(&self) -> &[u8] {
        &self.event
    }
}

/// A trait defining operations for a trace pipe buffer.
pub trait TracePipeOps {
    /// Returns the first record in the trace pipe buffer without removing it.
    fn peek(&self) -> Option<&TracePipeRecord>;

    /// Remove and return the first record in the trace pipe buffer.
    fn pop(&mut self) -> Option<TracePipeRecord>;

    /// Whether the trace pipe buffer is empty.
    fn is_empty(&self) -> bool;
}

/// A raw trace pipe buffer: a fixed-capacity **circular buffer** of records.
///
/// Once [`Self::prealloc`]'d, [`Self::push_record_bytes`] is **allocation-free**
/// (it overwrites the oldest slot's payload buffer in place) and O(1) — required
/// for the function tracer, whose handler runs on every kernel function call and
/// must not allocate (a traced allocator internal holding the alloc lock would
/// otherwise deadlock the push). `slots` is the backing store, used circularly
/// via `head` (oldest live record) and `count` (number of live records).
pub struct TracePipeRaw {
    max_record: usize,
    slots: Vec<TracePipeRecord>,
    head: usize,
    count: usize,
}

impl TracePipeRaw {
    /// Create a new TracePipeRaw with the specified maximum number of records.
    pub const fn new(max_record: usize) -> Self {
        Self {
            max_record,
            slots: Vec::new(),
            head: 0,
            count: 0,
        }
    }

    /// Pre-allocate all `max_record` slots (each with a `slot_capacity`-byte
    /// payload buffer) so the hot push path never allocates. Call once from a
    /// process context (e.g. at init) before high-rate tracing. Idempotent.
    pub fn prealloc(&mut self, slot_capacity: usize) {
        if self.slots.is_empty() && self.max_record > 0 {
            self.slots = (0..self.max_record)
                .map(|_| TracePipeRecord::new(0, 0, Vec::with_capacity(slot_capacity)))
                .collect();
            self.head = 0;
            self.count = 0;
        }
    }

    /// Resize the ring, discarding its contents (kept simple — resizing is rare).
    pub fn set_max_record(&mut self, max_record: usize) {
        self.max_record = max_record;
        self.slots = Vec::new();
        self.head = 0;
        self.count = 0;
    }

    /// Push a new event into the trace pipe buffer without metadata.
    pub fn push_event(&mut self, event: Vec<u8>) {
        self.push_record_bytes(0, 0, &event);
    }

    /// Push a new event record (owned buffer). Kept for API compatibility; copies
    /// into the reused slot, so callers with a `&[u8]` should prefer
    /// [`Self::push_record_bytes`] to avoid the caller-side allocation.
    pub fn push_record(&mut self, timestamp: u64, cpu_id: u32, event: Vec<u8>) {
        self.push_record_bytes(timestamp, cpu_id, &event);
    }

    /// Allocation-free push: overwrite the next circular slot in place. If the
    /// ring was not [`Self::prealloc`]'d, it is grown once here (only safe in a
    /// context that may allocate — the function tracer preallocs beforehand).
    pub fn push_record_bytes(&mut self, timestamp: u64, cpu_id: u32, data: &[u8]) {
        if self.max_record == 0 {
            return;
        }
        if self.slots.is_empty() {
            self.prealloc(data.len().max(64));
        }
        let slot = (self.head + self.count) % self.max_record;
        self.slots[slot].set(timestamp, cpu_id, data);
        if self.count == self.max_record {
            self.head = (self.head + 1) % self.max_record; // overwrote the oldest
        } else {
            self.count += 1;
        }
    }

    /// The number of live records currently in the trace pipe buffer.
    pub fn event_count(&self) -> usize {
        self.count
    }

    /// Clear the trace pipe buffer (keeps the preallocated slots for reuse).
    pub fn clear(&mut self) {
        self.head = 0;
        self.count = 0;
    }

    /// Create a snapshot of the current state of the trace pipe buffer.
    pub fn snapshot(&self) -> TracePipeSnapshot {
        let mut out = Vec::with_capacity(self.count);
        for i in 0..self.count {
            out.push(self.slots[(self.head + i) % self.max_record].clone());
        }
        TracePipeSnapshot::new(out)
    }

    /// Get the maximum number of records allowed in the trace pipe buffer.
    pub fn max_record(&self) -> usize {
        self.max_record
    }
}

impl TracePipeOps for TracePipeRaw {
    fn peek(&self) -> Option<&TracePipeRecord> {
        (self.count > 0).then(|| &self.slots[self.head])
    }

    fn pop(&mut self) -> Option<TracePipeRecord> {
        if self.count == 0 {
            return None;
        }
        // The reader discards this value; return a clone and keep the slot's
        // buffer for reuse. Advancing is O(1).
        let rec = self.slots[self.head].clone();
        self.head = (self.head + 1) % self.max_record;
        self.count -= 1;
        Some(rec)
    }

    fn is_empty(&self) -> bool {
        self.count == 0
    }
}

/// A snapshot of the trace pipe buffer at a specific point in time.
#[derive(Debug)]
pub struct TracePipeSnapshot(Vec<TracePipeRecord>);

impl TracePipeSnapshot {
    /// Create a new TracePipeSnapshot with the given event buffer.
    pub fn new(event_buf: Vec<TracePipeRecord>) -> Self {
        Self(event_buf)
    }

    /// The formatted string representation to be used as a header for the trace pipe output.
    pub fn default_fmt_str(&self) -> String {
        let show = "#
#
#                                _-----=> irqs-off/BH-disabled
#                               / _----=> need-resched
#                              | / _---=> hardirq/softirq
#                              || / _--=> preempt-depth
#                              ||| / _-=> migrate-disable
#                              |||| /     delay
#           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
#              | |         |   |||||     |         |
";
        format!(
            "# tracer: nop\n#\n# entries-in-buffer/entries-written: {}/{}   #P:32\n{}",
            self.0.len(),
            self.0.len(),
            show
        )
    }
}

impl TracePipeOps for TracePipeSnapshot {
    fn peek(&self) -> Option<&TracePipeRecord> {
        self.0.first()
    }

    fn pop(&mut self) -> Option<TracePipeRecord> {
        if self.0.is_empty() {
            None
        } else {
            Some(self.0.remove(0))
        }
    }

    fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

/// A cache for storing command line arguments for each trace point.
///
/// See <https://www.kernel.org/doc/Documentation/trace/ftrace.txt>
pub struct TraceCmdLineCache {
    // cmdline: Vec<(u32, [u8; 16])>,
    cmdline: LruCache<u32, String>,
}

impl TraceCmdLineCache {
    /// Create a new TraceCmdLineCache with the specified maximum number of records.
    pub fn new(max_record: NonZero<usize>) -> Self {
        Self {
            cmdline: LruCache::new(max_record),
        }
    }

    /// Insert a command line argument for a trace point.
    ///
    /// If the command line exceeds 16 bytes, it will be truncated.
    /// If the cache exceeds the maximum record limit, the oldest entry will be removed.
    pub fn insert(&mut self, id: u32, cmdline: &str) {
        const MAX_CMDLINE_LEN: usize = 16;
        let (cmdline, _) = cmdline.split_at(MAX_CMDLINE_LEN.min(cmdline.len()));
        let line = format!("{} {}\n", id, cmdline);
        self.cmdline.put(id, line);
    }

    /// Get the command line argument for a trace point.
    pub fn get(&self, id: u32) -> Option<&str> {
        self.cmdline
            .iter()
            .find(|(key, _)| **key == id)
            .map(|(_, value)| {
                let line = value.as_str();
                line.splitn(2, ' ').nth(1).unwrap().trim_end_matches('\n')
            })
    }

    /// Set the maximum length for command line arguments.
    pub fn set_max_record(&mut self, max_len: NonZero<usize>) {
        self.cmdline.resize(max_len);
    }

    /// Get the maximum number of records in the cache.
    pub fn max_record(&self) -> usize {
        self.cmdline.cap().get()
    }

    /// Create a snapshot of the current state of the command line cache.
    pub fn snapshot(&self) -> TraceCmdLineCacheSnapshot {
        let cmdline = self
            .cmdline
            .iter()
            .map(|(_, value)| value)
            .cloned()
            .collect();
        TraceCmdLineCacheSnapshot::new(cmdline)
    }
}

/// A snapshot of the command line cache at a specific point in time.
#[derive(Debug)]
pub struct TraceCmdLineCacheSnapshot(Vec<String>);

impl TraceCmdLineCacheSnapshot {
    /// Create a new TraceCmdLineCacheSnapshot with the given command line entries.
    pub fn new(cmdline: Vec<String>) -> Self {
        Self(cmdline)
    }

    /// Return the first command line entry in the cache.
    pub fn peek(&self) -> Option<&String> {
        self.0.first()
    }

    /// Remove and return the first command line entry in the cache.
    pub fn pop(&mut self) -> Option<String> {
        if self.0.is_empty() {
            None
        } else {
            Some(self.0.remove(0))
        }
    }
}

/// A parser for trace entries that formats them into human-readable strings.
pub struct TraceEntryParser;

impl TraceEntryParser {
    /// Parse the trace entry and return a formatted string.
    pub fn parse<K: KernelTraceOps>(
        tracepoint_map: &TracePointMap<K>,
        cmdline_cache: &TraceCmdLineCache,
        record: &TracePipeRecord,
    ) -> String {
        let entry = record.event();
        let trace_entry = unsafe { &*(entry.as_ptr() as *const TraceEntry) };
        let id = trace_entry.common_type as u32;
        let offset = core::mem::size_of::<TraceEntry>();
        let payload = &entry[offset..];
        // Resolve the record's event: a compile-time tracepoint from the static
        // map, else a runtime facility's dynamic event (kprobe / function tracer)
        // via the `dynamic_event` hook, else a neutral placeholder. Never panic —
        // a dynamic id is expected here, not a bug.
        let (name, body) = match tracepoint_map.get(&id) {
            Some(tracepoint) => (
                tracepoint.name().to_string(),
                tracepoint.fmt_func()(payload),
            ),
            None => K::dynamic_event(id, payload)
                .unwrap_or_else(|| (String::from("unknown"), format!("id={id}"))),
        };

        let time = record.timestamp();
        let cpu_id = record.cpu_id();

        // Copy the packed field to a local variable to avoid unaligned reference
        let pid = trace_entry.common_pid;
        let pname = cmdline_cache
            .get(trace_entry.common_pid as u32)
            .unwrap_or("<...>");

        let secs = time / 1_000_000_000;
        let usec_rem = time % 1_000_000_000 / 1000;

        format!(
            "{:>16}-{:<7} [{:03}] {} {:5}.{:06}: {}({})\n",
            pname,
            pid,
            cpu_id,
            trace_entry.trace_print_lat_fmt(),
            secs,
            usec_rem,
            name,
            body
        )
    }
}
