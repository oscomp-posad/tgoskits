//! Wakeup-latency profiling (feature `wakeprof`).
//!
//! Measures **wake-to-run latency**: from the moment a blocked task is made
//! runnable (`put_task_with_state` Blocked→Ready) to the moment it is actually
//! switched onto a CPU. Split by whether the wake crossed cores, because a board
//! A/B showed a cross-core wake costs ~1 ms on RK3588 while a local hand-off is
//! ~9 µs, and we need to localise where that ~1 ms goes.
//!
//! All counters are process-global relaxed atoms — this is a diagnostic build,
//! not a production path, and the feature is off by default (zero cost). Read the
//! rendered snapshot from userspace via `/proc/wakeprof`.

use core::sync::atomic::{AtomicU64, Ordering::Relaxed};

use alloc::string::String;

/// Local (same-CPU) wake-to-run: the woken task lands on the waker's own CPU.
static LOCAL_SUM_NS: AtomicU64 = AtomicU64::new(0);
static LOCAL_CNT: AtomicU64 = AtomicU64::new(0);
static LOCAL_MAX_NS: AtomicU64 = AtomicU64::new(0);

/// Cross-core wake-to-run: the woken task is enqueued on a different CPU than the
/// waker (pays the GIC SGI + `on_cpu` switch-out handshake).
static XCORE_SUM_NS: AtomicU64 = AtomicU64::new(0);
static XCORE_CNT: AtomicU64 = AtomicU64::new(0);
static XCORE_MAX_NS: AtomicU64 = AtomicU64::new(0);

/// Of the cross-core wakes, how many took the deferred `on_cpu` stash path (the
/// wakee was still finishing its switch-out on its owning CPU at wake time).
static XCORE_DEFER_CNT: AtomicU64 = AtomicU64::new(0);

#[inline]
fn now_ns() -> u64 {
    ax_hal::time::monotonic_time_nanos()
}

/// Stamp the wake time on a task that just became runnable. Returns the timestamp
/// to store on the task (0 is used as "no pending wake", so bump a zero to 1 ns).
#[inline]
pub(crate) fn stamp_wake() -> u64 {
    now_ns().max(1)
}

/// Record that a cross-core wake took the deferred (`on_cpu` still set) path.
#[inline]
pub(crate) fn note_deferred() {
    XCORE_DEFER_CNT.fetch_add(1, Relaxed);
}

/// Called when a task with a pending wake stamp is switched onto a CPU.
/// `wake_ns` is the stamp taken at ready time; `xcore` is whether that wake
/// crossed cores.
#[inline]
pub(crate) fn record_run(wake_ns: u64, xcore: bool) {
    if wake_ns == 0 {
        return;
    }
    let now = now_ns();
    let delta = now.saturating_sub(wake_ns);
    let (sum, cnt, max) = if xcore {
        (&XCORE_SUM_NS, &XCORE_CNT, &XCORE_MAX_NS)
    } else {
        (&LOCAL_SUM_NS, &LOCAL_CNT, &LOCAL_MAX_NS)
    };
    sum.fetch_add(delta, Relaxed);
    cnt.fetch_add(1, Relaxed);
    max.fetch_max(delta, Relaxed);
}

/// Zero all counters (so a benchmark can snapshot a clean interval).
pub fn reset() {
    for a in [
        &LOCAL_SUM_NS,
        &LOCAL_CNT,
        &LOCAL_MAX_NS,
        &XCORE_SUM_NS,
        &XCORE_CNT,
        &XCORE_MAX_NS,
        &XCORE_DEFER_CNT,
    ] {
        a.store(0, Relaxed);
    }
}

/// Render the current snapshot as text for `/proc/wakeprof`.
pub fn render() -> String {
    let lsum = LOCAL_SUM_NS.load(Relaxed);
    let lcnt = LOCAL_CNT.load(Relaxed);
    let lmax = LOCAL_MAX_NS.load(Relaxed);
    let xsum = XCORE_SUM_NS.load(Relaxed);
    let xcnt = XCORE_CNT.load(Relaxed);
    let xmax = XCORE_MAX_NS.load(Relaxed);
    let xdef = XCORE_DEFER_CNT.load(Relaxed);
    let lavg = if lcnt > 0 { lsum / lcnt } else { 0 };
    let xavg = if xcnt > 0 { xsum / xcnt } else { 0 };
    // ns → µs for readability; keep raw ns too.
    alloc::format!(
        "wake-to-run latency profile (ns)\n\
         local  count={lcnt} avg_ns={lavg} avg_us={} max_ns={lmax} max_us={}\n\
         xcore  count={xcnt} avg_ns={xavg} avg_us={} max_ns={xmax} max_us={}\n\
         xcore_deferred_count={xdef}\n",
        lavg / 1000,
        lmax / 1000,
        xavg / 1000,
        xmax / 1000,
    )
}
