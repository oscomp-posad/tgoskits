//! ftrace **function tracer** — the classic `current_tracer=function` path.
//!
//! Compiled only in an opt-in build (`STARRY_FUNCTION_TRACER=1`, which the build
//! tooling turns into `-Zpatchable-function-entry=2` + `--cfg function_tracer`):
//! every kernel function then starts with two `NOP`s, and the compiler records
//! the address of each such sled in the `__patchable_function_entries` section.
//! To trace a function we self-patch its two NOPs into `mov x9, x30 ; bl
//! ftrace_caller`; the trampoline calls [`ftrace_handler`], which writes a record
//! into the trace ring (`cat trace`).
//!
//! An empty `set_ftrace_filter` traces **every** function; a non-empty filter
//! traces only the named ones. Trace-all batch-patches all sleds in one
//! `stop_machine` (per-site patching would run one each — unusably slow). The
//! handler is allocation-free (it copies into the preallocated trace ring).
//!
//! Reentrancy (critical under trace-all): the trampoline is deliberately simple
//! (no guard); [`ftrace_handler`] arms a per-CPU busy flag so a traced callee of
//! the handler — its own callees are instrumented too — is dropped rather than
//! recursing. The flag MUST be armed using **only sled-free** operations (a raw
//! `DAIF` IRQ mask + the `#[inline]` per-CPU accessor): calling any *out-of-line*
//! function before the flag is set would, under trace-all, re-enter the handler
//! before it can guard and recurse unboundedly into a stack overflow (observed as
//! all cores wedged in the sync-exception vector). That is why the prologue uses
//! raw IRQ masking rather than `NoPreemptIrqSave`, whose
//! `new`/`acquire`/`disable_preempt` carry patchable sleds. `ftrace_handler` /
//! `ftrace_caller` are themselves never patched (guarded in [`patch_entry`]).

use alloc::{format, string::String, vec::Vec};
use core::sync::atomic::{AtomicBool, Ordering};

use ax_kspin::SpinNoPreempt;
use ax_memory_addr::VirtAddr;
use axfs_ng_vfs::{VfsError, VfsResult};
use ksym::KSYM_NAME_LEN;
use ktracepoint::KernelTraceOps;

use crate::{pseudofs::DirectRwFsFileOps, task::AsThread, tracepoint::KernelTraceAux};

/// Synthetic tracefs event id for function-tracer records. Below the dynamic
/// kprobe base (`0x8000`) and clear of the static tracepoint ids (`0..N`), and it
/// fits the 2-byte `common_type` field libtraceevent/the trace reader key on.
pub const FUNCTION_EVENT_ID: u32 = 0x7000;

// --- patchable-function-entry table (Increment A) ---------------------------

/// The linker (LLD) synthesises these bracketing symbols for the
/// C-identifier-named section that holds one pointer per instrumented function
/// (the address of its two-`NOP` patchable entry). Kept across `--gc-sections`
/// because the link uses `-znostart-stop-gc` and this module references them.
unsafe extern "C" {
    static __start___patchable_function_entries: u8;
    static __stop___patchable_function_entries: u8;
}

/// The patchable-function-entry pointers: one per instrumented function, each the
/// address of that function's two-`NOP` sled (i.e. the function entry).
fn sled_addrs() -> &'static [usize] {
    let start = (&raw const __start___patchable_function_entries) as usize;
    let stop = (&raw const __stop___patchable_function_entries) as usize;
    let len = stop.saturating_sub(start) / core::mem::size_of::<usize>();
    // SAFETY: contiguous array of `usize` entries between the linker symbols.
    unsafe { core::slice::from_raw_parts(start as *const usize, len) }
}

// --- the trampoline ---------------------------------------------------------

core::arch::global_asm!(
    r#"
    .section .text
    .global ftrace_caller
    .type ftrace_caller, @function
ftrace_caller:
    // On entry the patched site ran `mov x9, x30 ; bl ftrace_caller`, so:
    //   x9  = the traced function's original return address (its caller)
    //   x30 = the address just past the `bl` = the traced function's body
    // Save the caller-saved GPRs the handler may clobber (x0-x18) + x9 + x30.
    sub  sp, sp, #(16 * 10)
    stp  x0,  x1,  [sp, #(16 * 0)]
    stp  x2,  x3,  [sp, #(16 * 1)]
    stp  x4,  x5,  [sp, #(16 * 2)]
    stp  x6,  x7,  [sp, #(16 * 3)]
    stp  x8,  x9,  [sp, #(16 * 4)]
    stp  x10, x11, [sp, #(16 * 5)]
    stp  x12, x13, [sp, #(16 * 6)]
    stp  x14, x15, [sp, #(16 * 7)]
    stp  x16, x17, [sp, #(16 * 8)]
    stp  x18, x30, [sp, #(16 * 9)]
    sub  x0, x30, #8        // arg0 = ip  (the function entry: two insns before body)
    mov  x1, x9             // arg1 = parent ip (the caller)
    bl   ftrace_handler
    ldp  x0,  x1,  [sp, #(16 * 0)]
    ldp  x2,  x3,  [sp, #(16 * 1)]
    ldp  x4,  x5,  [sp, #(16 * 2)]
    ldp  x6,  x7,  [sp, #(16 * 3)]
    ldp  x8,  x9,  [sp, #(16 * 4)]
    ldp  x10, x11, [sp, #(16 * 5)]
    ldp  x12, x13, [sp, #(16 * 6)]
    ldp  x14, x15, [sp, #(16 * 7)]
    ldp  x16, x17, [sp, #(16 * 8)]
    ldp  x18, x30, [sp, #(16 * 9)]
    add  sp, sp, #(16 * 10)
    mov  x16, x30          // x16 = body (x16/IP0 is scratch, dead at fn entry)
    mov  x30, x9           // restore the traced function's original return addr
    br   x16               // continue into the function body
    .size ftrace_caller, . - ftrace_caller
"#
);

unsafe extern "C" {
    fn ftrace_caller();
}

// --- the handler ------------------------------------------------------------

/// Set while `current_tracer=function`. Gates the handler cheaply.
static TRACING_ON: AtomicBool = AtomicBool::new(false);
/// Per-CPU reentrancy guard: a traced callee of the handler (the handler's own
/// callees are instrumented too) sees this CPU's flag set and drops its record
/// rather than recursing. Per-CPU (not global), so every core traces
/// independently; the handler masks IRQs (raw `DAIF`) across the guarded region
/// so the current-CPU slot can't migrate and no IRQ-context trace nests.
#[ax_percpu::def_percpu]
static IN_HANDLER: bool = false;

/// Raw local-IRQ save+disable — the exact `DAIF` idiom from `kernel_guard`'s
/// aarch64 arch module, replicated here as an always-inlined leaf so it carries
/// **no** patchable sled. The crate's out-of-line `NoPreemptIrqSave`/`IrqSave`
/// would re-enter [`ftrace_handler`] under trace-all before the reentrancy guard
/// is armed (see the module docs).
#[inline(always)]
fn local_irq_save_raw() -> usize {
    let flags: usize;
    // SAFETY: reads `DAIF` and masks the `I` bit; no memory/stack effects.
    unsafe {
        core::arch::asm!(
            "mrs {}, daif; msr daifset, #2",
            out(reg) flags,
            options(nomem, nostack, preserves_flags)
        );
    }
    flags
}

/// Restore `DAIF` saved by [`local_irq_save_raw`]. Always-inlined (sled-free).
#[inline(always)]
fn local_irq_restore_raw(flags: usize) {
    // SAFETY: writes back the previously saved `DAIF` flags.
    unsafe {
        core::arch::asm!(
            "msr daif, {}",
            in(reg) flags,
            options(nomem, nostack, preserves_flags)
        );
    }
}

/// Called by [`ftrace_caller`] on every hit of a patched function.
///
/// `ip` is the traced function's entry; `parent` is its caller's return address.
/// Writes a `{common header, ip, parent}` record into the trace ring. Must not
/// panic or block: it runs in the traced function's context with the guard held.
///
/// The reentrancy guard is armed with **sled-free** ops only (a raw IRQ mask +
/// the inlined per-CPU accessor) *before* any out-of-line call. Under trace-all
/// every function is patched, so calling e.g. `NoPreemptIrqSave::new` here —
/// before the guard — would re-enter this handler unbounded and overflow the
/// stack. Raw IRQ masking also pins us to this CPU (no timer ⇒ no preemption), so
/// the per-CPU slot is stable for the guarded region.
#[unsafe(no_mangle)]
extern "C" fn ftrace_handler(ip: usize, parent: usize) {
    if !TRACING_ON.load(Ordering::Relaxed) {
        return;
    }
    // Mask IRQs with a raw, sled-free op, then arm the per-CPU guard, BEFORE any
    // out-of-line (patchable) call — otherwise trace-all recurses here.
    let flags = local_irq_save_raw();
    // SAFETY: IRQs are off, so this CPU's slot is stable for the whole handler.
    let busy = unsafe { IN_HANDLER.current_ref_mut_raw() };
    if *busy {
        local_irq_restore_raw(flags);
        return;
    }
    *busy = true;
    // Attribute to the running thread; a kernel task without a `Thread` uses pid 0
    // (no `as_thread()` panic, unlike the tracepoint fire path).
    let pid = ax_task::current()
        .try_as_thread()
        .map(|t| t.proc_data.proc.pid())
        .unwrap_or(0);
    let mut buf = [0u8; 24];
    buf[0..2].copy_from_slice(&(FUNCTION_EVENT_ID as u16).to_ne_bytes()); // common_type
    buf[4..8].copy_from_slice(&(pid as i32).to_ne_bytes()); // common_pid
    buf[8..16].copy_from_slice(&(ip as u64).to_ne_bytes());
    buf[16..24].copy_from_slice(&(parent as u64).to_ne_bytes());
    KernelTraceAux::trace_pipe_push_raw_record(&buf);
    *busy = false;
    local_irq_restore_raw(flags);
}

/// Symbolize a kernel address to its function name (for `trace` rendering).
fn symbolize(addr: u64) -> String {
    let mut buf = [0u8; KSYM_NAME_LEN];
    crate::pseudofs::proc::KALLSYMS
        .get()
        .and_then(|t| t.lookup_address(addr, &mut buf))
        .map(|(name, ..)| String::from(name))
        .unwrap_or_else(|| format!("0x{addr:x}"))
}

/// Render a function-tracer record for `trace`/`trace_pipe` (called from the
/// ktracepoint fork's `dynamic_event`): `funcname <-parent`.
pub fn render_record(payload: &[u8]) -> Option<(String, String)> {
    let ip = u64::from_ne_bytes(payload.get(0..8)?.try_into().ok()?);
    let parent = u64::from_ne_bytes(payload.get(8..16)?.try_into().ok()?);
    Some((symbolize(ip), format!("<-{}", symbolize(parent))))
}

// --- self-patching ----------------------------------------------------------

const NOP: u32 = 0xd503_201f;
/// `mov x9, x30` (`orr x9, xzr, x30`): saves the return addr before `bl` clobbers it.
const MOV_X9_X30: u32 = 0xaa1e_03e9;

/// Encode `bl <to>` from address `from`. `bl` reaches ±128 MiB; kernel text is far
/// smaller, so the offset always fits.
fn bl_insn(from: usize, to: usize) -> u32 {
    let off = (to as isize - from as isize) >> 2;
    0x9400_0000 | ((off as u32) & 0x03ff_ffff)
}

fn write_insn(addr: usize, insn: u32) {
    let _ = crate::mm::write_kernel_text(VirtAddr::from_usize(addr), &insn.to_le_bytes());
}

/// Whether the two words at `entry` are the untouched `NOP; NOP` sled (i.e. a real
/// patchable entry that is not currently armed).
fn is_nop_sled(entry: usize) -> bool {
    // SAFETY: `entry` comes from the patchable-entry table (valid kernel text).
    unsafe {
        core::ptr::read_volatile(entry as *const u32) == NOP
            && core::ptr::read_volatile((entry + 4) as *const u32) == NOP
    }
}

/// Arm (`on`) or disarm a patchable entry, writing the two words in the order that
/// keeps every intermediate state safe: enabling writes `mov` before `bl` (a lone
/// `mov x9,x30` is harmless); disabling writes `bl`→`nop` first.
fn patch_entry(entry: usize, on: bool) {
    // Never patch the tracer's own trampoline/handler — it would recurse before
    // the Rust guard runs.
    if in_ftrace_module(entry) {
        return;
    }
    if on {
        write_insn(entry, MOV_X9_X30);
        write_insn(entry + 4, bl_insn(entry + 4, ftrace_caller as usize));
    } else {
        write_insn(entry + 4, NOP);
        write_insn(entry, NOP);
    }
}

/// Arm (`on`) or disarm **every** eligible patchable entry in one `stop_machine`
/// (via [`crate::mm::patch_kernel_text_batch`]) — `current_tracer=function` with
/// an empty filter. Per-site patching would run one `stop_machine` each (parking
/// every core), which is unusably slow for the ~11 k entries. Only the tracer's
/// own trampoline/handler are skipped: the handler is allocation-free and holds a
/// per-CPU guard, so every other function (allocator included) is safe to trace.
fn set_all(on: bool) {
    let mut sleds: Vec<usize> = Vec::new();
    for &entry in sled_addrs() {
        if in_ftrace_module(entry) {
            continue;
        }
        // When arming, only touch a genuine, unarmed NOP sled.
        if on && !is_nop_sled(entry) {
            continue;
        }
        sleds.push(entry);
    }
    let (Some(&min), Some(&max)) = (sleds.iter().min(), sleds.iter().max()) else {
        return;
    };
    let range_len = (max - min) + 8;
    let caller = ftrace_caller as usize;
    // All cores are parked for the whole closure, so intermediate two-word states
    // are never executed — no per-site ordering needed.
    let _ = crate::mm::patch_kernel_text_batch(VirtAddr::from_usize(min), range_len, || {
        for &e in &sleds {
            let (i0, i1) = if on {
                (MOV_X9_X30, bl_insn(e + 4, caller))
            } else {
                (NOP, NOP)
            };
            // SAFETY: the range [min, max+8) is writable for this closure and each
            // `e` is a 4-byte-aligned instruction slot within it.
            unsafe {
                core::ptr::write(e as *mut u32, i0);
                core::ptr::write((e + 4) as *mut u32, i1);
            }
        }
    });
}

/// Guard: the trampoline and handler must never be instrumented targets (they'd
/// recurse before the Rust guard runs). Compares the containing symbol's start.
fn in_ftrace_module(entry: usize) -> bool {
    let Some(kt) = crate::pseudofs::proc::KALLSYMS.get() else {
        return false;
    };
    let mut buf = [0u8; KSYM_NAME_LEN];
    let start_of =
        |a: u64, buf: &mut [u8; KSYM_NAME_LEN]| kt.lookup_address(a, buf).map(|(_, s, ..)| s);
    let Some(entry_start) = start_of(entry as u64, &mut buf) else {
        return false;
    };
    start_of(ftrace_caller as usize as u64, &mut buf) == Some(entry_start)
        || start_of(ftrace_handler as usize as u64, &mut buf) == Some(entry_start)
}

// --- the filter + tracer state ----------------------------------------------

/// Function entries to arm when `current_tracer=function`. Empty by default
/// (v1 requires an explicit `set_ftrace_filter`); populated by resolving names to
/// their patchable entry. `SpinNoPreempt` — read/written only from process
/// context (tracefs writes), never the handler.
static FILTER: SpinNoPreempt<Vec<usize>> = SpinNoPreempt::new(Vec::new());

/// `set_ftrace_filter` write: resolve each whitespace/newline-separated name to
/// its patchable entry and add it to the filter. `> set_ftrace_filter` clears.
pub fn set_filter(text: &str) {
    let mut filter = FILTER.lock();
    if text.trim().is_empty() {
        filter.clear();
        return;
    }
    let Some(kt) = crate::pseudofs::proc::KALLSYMS.get() else {
        return;
    };
    for name in text.split_whitespace() {
        if let Some(addr) = kt.lookup_name(name) {
            let entry = addr as usize;
            // Only accept a genuine, unarmed patchable entry.
            if is_nop_sled(entry) && !filter.contains(&entry) {
                filter.push(entry);
            }
        }
    }
}

/// Read-back of the current filter as symbol names, one per line.
pub fn filter_text() -> String {
    let filter = FILTER.lock();
    let mut out = String::new();
    for &entry in filter.iter() {
        out.push_str(&symbolize(entry as u64));
        out.push('\n');
    }
    out
}

/// `current_tracer` state: `true` while `function` is selected. An empty filter
/// traces **every** function (batch-patched); a non-empty filter traces only the
/// named functions. The `TRACING_ON` gate is set after arming / cleared before
/// disarming so the handler is inert while the text is inconsistent.
fn set_function_tracer(on: bool) {
    let empty = FILTER.lock().is_empty();
    if on {
        if empty {
            set_all(true);
        } else {
            let filter = FILTER.lock();
            for &entry in filter.iter() {
                patch_entry(entry, true);
            }
        }
        TRACING_ON.store(true, Ordering::Release);
    } else {
        TRACING_ON.store(false, Ordering::Release);
        if empty {
            set_all(false);
        } else {
            let filter = FILTER.lock();
            for &entry in filter.iter() {
                patch_entry(entry, false);
            }
        }
    }
}

/// `current_tracer` write: `function` arms the filtered entries, `nop` disarms.
pub fn set_current_tracer(name: &str) -> bool {
    match name.trim() {
        "function" => {
            set_function_tracer(true);
            true
        }
        "nop" => {
            set_function_tracer(false);
            true
        }
        _ => false,
    }
}

/// `current_tracer` read-back.
pub fn current_tracer() -> &'static str {
    if TRACING_ON.load(Ordering::Relaxed) {
        "function"
    } else {
        "nop"
    }
}

// --- tracefs files ----------------------------------------------------------

fn read_from(content: &[u8], buf: &mut [u8], offset: u64) -> VfsResult<usize> {
    let offset = offset as usize;
    if offset >= content.len() {
        return Ok(0);
    }
    let n = buf.len().min(content.len() - offset);
    buf[..n].copy_from_slice(&content[offset..offset + n]);
    Ok(n)
}

/// `available_tracers`.
pub struct AvailableTracersFile;
impl DirectRwFsFileOps for AvailableTracersFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        read_from(b"function nop\n", buf, offset)
    }
}

/// `current_tracer` — write `function`/`nop` to arm/disarm.
pub struct CurrentTracerFile;
impl DirectRwFsFileOps for CurrentTracerFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        read_from(format!("{}\n", current_tracer()).as_bytes(), buf, offset)
    }
    fn write_at(&self, buf: &[u8], _offset: u64) -> VfsResult<usize> {
        let text = core::str::from_utf8(buf).map_err(|_| VfsError::InvalidInput)?;
        if set_current_tracer(text) {
            Ok(buf.len())
        } else {
            Err(VfsError::InvalidInput)
        }
    }
}

/// `set_ftrace_filter` — write function names to trace (empty write clears).
pub struct SetFtraceFilterFile;
impl DirectRwFsFileOps for SetFtraceFilterFile {
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize> {
        read_from(filter_text().as_bytes(), buf, offset)
    }
    fn write_at(&self, buf: &[u8], _offset: u64) -> VfsResult<usize> {
        let text = core::str::from_utf8(buf).map_err(|_| VfsError::InvalidInput)?;
        set_filter(text);
        Ok(buf.len())
    }
}

/// One-time init at boot: report the instrumented-function count so the section
/// is confirmed readable.
pub fn init() {
    warn!(
        "ftrace: function tracer available, {} patchable function entries",
        sled_addrs().len()
    );
}
