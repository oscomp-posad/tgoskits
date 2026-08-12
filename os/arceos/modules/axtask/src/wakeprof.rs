//! Wakeup-latency profiling (feature `wakeprof`).
//!
//! Decomposes the cross-core wake path end to end so one board run localises the
//! ~1 ms cross-core wake cost on RK3588:
//!
//! ```text
//!  waker: ready(stamp) ── enqueue+kick ──▶ IPI send ─┐
//!                                                     │  IPI_DELIVERY (kick→handler)
//!  target(idle,WFI): SGI ─▶ ipi handler(clear+resched)┘
//!                          │  PICK_AFTER_HANDLER (handler→switch-in)
//!  target: idle loop yield ─▶ switch_to(consume stamp) = wake-to-run total
//! ```
//!
//! - `LOCAL` / `XCORE_IDLE` / `XCORE_BUSY`: wake-to-run (ready→run), split by whether
//!   the target was the waker's CPU, a cross-core idle CPU, or a cross-core busy CPU.
//! - `IPI_DELIVERY`: reschedule SGI send → the target's IPI handler runs (delivery +
//!   WFI-exit).
//! - `PICK_AFTER_HANDLER`: IPI handler ran → the woken task is actually switched in.
//! - `ipi_sent` / `ipi_suppressed`: whether the kick actually sent an SGI, or was
//!   coalesced away because `REMOTE_RESCHEDULE_PENDING` was already set.
//!
//! All counters are process-global relaxed atoms (diagnostic build only; feature off
//! by default = zero cost). Read the rendered snapshot from `/proc/wakeprof`.

use core::sync::atomic::{AtomicU64, Ordering::Relaxed};

use alloc::string::String;

/// log2 latency buckets: bucket `k` holds samples with `delta_ns` in `[2^(k-1), 2^k)`
/// (bucket 0 = 0 ns). 32 buckets cover up to ~2.1 s. Percentiles are read out of the
/// cumulative histogram in [`Cat::pctl_ns`].
const NUM_BUCKETS: usize = 32;
const NCPU: usize = crate::build_info::CPU_CAPACITY;

struct Cat {
    sum_ns: AtomicU64,
    cnt: AtomicU64,
    max_ns: AtomicU64,
    hist: [AtomicU64; NUM_BUCKETS],
}

impl Cat {
    const fn new() -> Self {
        Self {
            sum_ns: AtomicU64::new(0),
            cnt: AtomicU64::new(0),
            max_ns: AtomicU64::new(0),
            hist: [const { AtomicU64::new(0) }; NUM_BUCKETS],
        }
    }
    fn record(&self, delta: u64) {
        self.sum_ns.fetch_add(delta, Relaxed);
        self.cnt.fetch_add(1, Relaxed);
        self.max_ns.fetch_max(delta, Relaxed);
        let b = if delta == 0 {
            0
        } else {
            (64 - delta.leading_zeros() as usize).min(NUM_BUCKETS - 1)
        };
        self.hist[b].fetch_add(1, Relaxed);
    }
    fn reset(&self) {
        self.sum_ns.store(0, Relaxed);
        self.cnt.store(0, Relaxed);
        self.max_ns.store(0, Relaxed);
        for h in &self.hist {
            h.store(0, Relaxed);
        }
    }
    /// Percentile latency estimate (ns) = upper bound of the bucket the fraction
    /// falls in. `pct` is 0..=100.
    fn pctl_ns(&self, pct: u64) -> u64 {
        let total = self.cnt.load(Relaxed);
        if total == 0 {
            return 0;
        }
        let target = total * pct / 100;
        let mut cum = 0u64;
        for (k, h) in self.hist.iter().enumerate() {
            cum += h.load(Relaxed);
            if cum >= target {
                return if k == 0 { 0 } else { 1u64 << k };
            }
        }
        self.max_ns.load(Relaxed)
    }
}

/// Wake-to-run, split by target relative to the waker.
static LOCAL: Cat = Cat::new();
static XCORE_IDLE: Cat = Cat::new();
static XCORE_BUSY: Cat = Cat::new();

/// Reschedule-SGI send → target IPI handler runs.
static IPI_DELIVERY: Cat = Cat::new();
/// Target IPI handler ran → the woken task is switched in.
static PICK_AFTER_HANDLER: Cat = Cat::new();

/// Cross-core wakes that took the deferred `on_cpu` stash path.
static XCORE_DEFER_CNT: AtomicU64 = AtomicU64::new(0);
/// Reschedule kicks that actually sent an SGI vs were coalesced away.
static IPI_SENT: AtomicU64 = AtomicU64::new(0);
static IPI_SUPPRESSED: AtomicU64 = AtomicU64::new(0);

/// Per-CPU flag: is CPU `i` currently halted in WFI in the idle loop?
static IN_WFI: [core::sync::atomic::AtomicBool; NCPU] =
    [const { core::sync::atomic::AtomicBool::new(false) }; NCPU];
/// Of the reschedule SGIs sent, how many targeted a CPU that was in WFI vs awake.
/// If most target WFI CPUs *and* `ipi_deliver` is ~1 ms, the SGI genuinely fails to
/// wake WFI (hardware). If most target awake CPUs, the "idle" target was actually
/// busy/transitioning → placement/queueing, which wake_affine already addresses.
static SGI_TO_WFI: AtomicU64 = AtomicU64::new(0);
static SGI_TO_AWAKE: AtomicU64 = AtomicU64::new(0);

/// Wake-placement decision trace: a ring buffer recording, for each wakeup, which
/// core `select_wake_run_queue` chose and why. Bounded so it captures the handful of
/// wakes a low-wake workload (e.g. a mem_bw2 barrier release) makes without flooding.
/// Each slot packs one decision (see `note_wake_place`). Dumped by `render()` (i.e.
/// `cat /proc/wakeprof`), cleared by `reset()` (`cat /proc/wakeprof_reset`).
const WT_N: usize = 512;
static WT_BUF: [AtomicU64; WT_N] = [const { AtomicU64::new(0) }; WT_N];
static WT_HEAD: AtomicU64 = AtomicU64::new(0);

/// Record one wake-placement decision. `prev` is the wakee's previous CPU (`None` if
/// not usable), `occ_prev` its occupancy at decision time, `chosen` the target core.
/// `flags` bit0=wake-spread fired, bit1=prefer-idle-prev fired, bit2=prev was idle,
/// bit3=wake_affine eligible. `wakee` is the woken task id (low 24 bits kept).
/// Packing (u64): [63]valid [59..63]waker [53..59]prev(63=none) [45..53]occ_prev(cap255)
/// [39..45]chosen [35..39]flags [0..24]wakee_lo.
pub(crate) fn note_wake_place(
    waker: usize,
    prev: Option<usize>,
    occ_prev: usize,
    chosen: usize,
    flags: u8,
    wakee: u64,
) {
    let prev6 = prev.map(|p| (p as u64) & 0x3f).unwrap_or(0x3f);
    let packed = (1u64 << 63)
        | (((waker as u64) & 0xf) << 59)
        | (prev6 << 53)
        | (((occ_prev.min(255)) as u64) << 45)
        | (((chosen as u64) & 0x3f) << 39)
        | (((flags as u64) & 0xf) << 35)
        | (wakee & 0xff_ffff);
    let i = (WT_HEAD.fetch_add(1, Relaxed) as usize) % WT_N;
    WT_BUF[i].store(packed, Relaxed);
}

/// Idle loop entering/leaving WFI on CPU `cpu`.
#[inline]
pub(crate) fn wfi_enter(cpu: usize) {
    if cpu < NCPU {
        IN_WFI[cpu].store(true, Relaxed);
    }
}
#[inline]
pub(crate) fn wfi_exit(cpu: usize) {
    if cpu < NCPU {
        IN_WFI[cpu].store(false, Relaxed);
    }
}

/// Per-CPU kick timestamp: set when a reschedule SGI is sent to CPU `i`, read (and
/// cleared) when CPU `i`'s IPI handler runs → measures IPI delivery latency.
static KICK_TS: [AtomicU64; NCPU] = [const { AtomicU64::new(0) }; NCPU];
/// Per-CPU handler timestamp: set when CPU `i`'s IPI handler runs, read (and cleared)
/// at the next switch-in on CPU `i` → measures handler→pick latency.
static HANDLER_TS: [AtomicU64; NCPU] = [const { AtomicU64::new(0) }; NCPU];

#[inline]
fn now_ns() -> u64 {
    ax_hal::time::monotonic_time_nanos()
}

/// Stamp the wake time on a task that just became runnable (0 is "no pending wake",
/// so floor at 1 ns).
#[inline]
pub(crate) fn stamp_wake() -> u64 {
    now_ns().max(1)
}

/// A cross-core wake took the deferred (`on_cpu` still set) path.
#[inline]
pub(crate) fn note_deferred() {
    XCORE_DEFER_CNT.fetch_add(1, Relaxed);
}

/// A reschedule SGI was actually sent to `cpu` (pending flag flipped false→true).
#[inline]
pub(crate) fn note_ipi_kick(cpu: usize) {
    IPI_SENT.fetch_add(1, Relaxed);
    if cpu < NCPU {
        KICK_TS[cpu].store(now_ns().max(1), Relaxed);
        if IN_WFI[cpu].load(Relaxed) {
            SGI_TO_WFI.fetch_add(1, Relaxed);
        } else {
            SGI_TO_AWAKE.fetch_add(1, Relaxed);
        }
    }
}

/// A reschedule kick was coalesced away (pending flag already set → no new SGI).
#[inline]
pub(crate) fn note_ipi_suppressed() {
    IPI_SUPPRESSED.fetch_add(1, Relaxed);
}

/// CPU `cpu`'s reschedule IPI handler is running now. Closes the IPI-delivery
/// interval and opens the handler→pick interval.
#[inline]
pub(crate) fn note_ipi_handler(cpu: usize) {
    if cpu >= NCPU {
        return;
    }
    let now = now_ns();
    let kick = KICK_TS[cpu].swap(0, Relaxed);
    if kick != 0 {
        IPI_DELIVERY.record(now.saturating_sub(kick));
    }
    HANDLER_TS[cpu].store(now, Relaxed);
}

/// A task is being switched onto `cpu`. Closes the handler→pick interval (if this
/// pick followed an IPI handler).
#[inline]
pub(crate) fn note_pick(cpu: usize) {
    if cpu >= NCPU {
        return;
    }
    let h = HANDLER_TS[cpu].swap(0, Relaxed);
    if h != 0 {
        PICK_AFTER_HANDLER.record(now_ns().saturating_sub(h));
    }
}

/// A task with a pending wake stamp is switched onto a CPU; record wake-to-run by
/// category (0=local, 1=xcore-idle, 2=xcore-busy).
#[inline]
pub(crate) fn record_run(wake_ns: u64, cat: u8) {
    if wake_ns == 0 {
        return;
    }
    let delta = now_ns().saturating_sub(wake_ns);
    match cat {
        1 => &XCORE_IDLE,
        2 => &XCORE_BUSY,
        _ => &LOCAL,
    }
    .record(delta);
}

/// Zero all counters (so a benchmark can snapshot a clean interval).
pub fn reset() {
    for c in [
        &LOCAL,
        &XCORE_IDLE,
        &XCORE_BUSY,
        &IPI_DELIVERY,
        &PICK_AFTER_HANDLER,
    ] {
        c.reset();
    }
    XCORE_DEFER_CNT.store(0, Relaxed);
    IPI_SENT.store(0, Relaxed);
    IPI_SUPPRESSED.store(0, Relaxed);
    SGI_TO_WFI.store(0, Relaxed);
    SGI_TO_AWAKE.store(0, Relaxed);
    for i in 0..NCPU {
        KICK_TS[i].store(0, Relaxed);
        HANDLER_TS[i].store(0, Relaxed);
    }
    WT_HEAD.store(0, Relaxed);
    for w in &WT_BUF {
        w.store(0, Relaxed);
    }
}

fn render_cat(name: &str, c: &Cat) -> String {
    let cnt = c.cnt.load(Relaxed);
    let avg = if cnt > 0 { c.sum_ns.load(Relaxed) / cnt } else { 0 };
    alloc::format!(
        "{name}  count={cnt} avg_us={} p50_us={} p90_us={} p99_us={} max_us={}\n",
        avg / 1000,
        c.pctl_ns(50) / 1000,
        c.pctl_ns(90) / 1000,
        c.pctl_ns(99) / 1000,
        c.max_ns.load(Relaxed) / 1000,
    )
}

/// Render the current snapshot as text for `/proc/wakeprof`.
pub fn render() -> String {
    let mut s = String::from("wake-to-run latency profile (percentiles = bucket upper bound)\n");
    s.push_str(&render_cat("local       ", &LOCAL));
    s.push_str(&render_cat("xcore_idle  ", &XCORE_IDLE));
    s.push_str(&render_cat("xcore_busy  ", &XCORE_BUSY));
    s.push_str("cross-core hop breakdown:\n");
    s.push_str(&render_cat("ipi_deliver ", &IPI_DELIVERY));
    s.push_str(&render_cat("pick_after_h", &PICK_AFTER_HANDLER));
    s.push_str(&alloc::format!(
        "ipi_sent={} ipi_suppressed={} xcore_deferred={} sgi_to_wfi={} sgi_to_awake={}\n",
        IPI_SENT.load(Relaxed),
        IPI_SUPPRESSED.load(Relaxed),
        XCORE_DEFER_CNT.load(Relaxed),
        SGI_TO_WFI.load(Relaxed),
        SGI_TO_AWAKE.load(Relaxed),
    ));
    // Wake-placement trace, oldest first. flags: S=wake-spread fired,
    // P=prefer-idle-prev fired, i=prev-idle, A=affine-eligible.
    s.push_str("wake-place trace (waker prev occ -> chosen [flags]):\n");
    let head = WT_HEAD.load(Relaxed) as usize;
    let start = head.saturating_sub(WT_N);
    for k in start..head {
        let p = WT_BUF[k % WT_N].load(Relaxed);
        if p >> 63 == 0 {
            continue;
        }
        let waker = (p >> 59) & 0xf;
        let prev = (p >> 53) & 0x3f;
        let occ = (p >> 45) & 0xff;
        let chosen = (p >> 39) & 0x3f;
        let f = (p >> 35) & 0xf;
        let tid = p & 0xff_ffff;
        let fl = alloc::format!(
            "{}{}{}{}",
            if f & 1 != 0 { "S" } else { "-" },
            if f & 2 != 0 { "P" } else { "-" },
            if f & 4 != 0 { "i" } else { "-" },
            if f & 8 != 0 { "A" } else { "-" },
        );
        let prevs = if prev == 0x3f {
            String::from("-")
        } else {
            alloc::format!("{prev}")
        };
        s.push_str(&alloc::format!(
            "WP tid={tid} w={waker} prev={prevs} occ={occ} -> cpu{chosen} [{fl}]\n"
        ));
    }
    s
}
