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

/// log2 latency buckets: bucket `k` holds wakes with `delta_ns` in `[2^(k-1), 2^k)`
/// (bucket 0 = 0 ns). 32 buckets cover up to ~2.1 s, enough for the oversubscription
/// tail. Percentiles are read out of the cumulative histogram in [`render`].
const NUM_BUCKETS: usize = 32;

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
    /// Percentile latency estimate (ns) = upper bound of the bucket the given
    /// fraction falls in. `pct` is 0..=100.
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
                // bucket k upper bound = 2^k ns (bucket 0 = ~0)
                return if k == 0 { 0 } else { 1u64 << k };
            }
        }
        self.max_ns.load(Relaxed)
    }
}

/// Local (same-CPU) wake-to-run: the woken task lands on the waker's own CPU.
static LOCAL: Cat = Cat::new();
/// Cross-core wake-to-run: the woken task is enqueued on a different CPU than the
/// waker (pays the GIC SGI + `on_cpu` switch-out handshake).
static XCORE: Cat = Cat::new();

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
    let delta = now_ns().saturating_sub(wake_ns);
    if xcore { &XCORE } else { &LOCAL }.record(delta);
}

/// Zero all counters (so a benchmark can snapshot a clean interval).
pub fn reset() {
    LOCAL.reset();
    XCORE.reset();
    XCORE_DEFER_CNT.store(0, Relaxed);
}

fn render_cat(name: &str, c: &Cat) -> String {
    let cnt = c.cnt.load(Relaxed);
    let avg = if cnt > 0 { c.sum_ns.load(Relaxed) / cnt } else { 0 };
    // Percentiles in µs (bucket upper bounds → coarse but distribution-true).
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
    s.push_str(&render_cat("local", &LOCAL));
    s.push_str(&render_cat("xcore", &XCORE));
    s.push_str(&alloc::format!(
        "xcore_deferred_count={}\n",
        XCORE_DEFER_CNT.load(Relaxed)
    ));
    s
}
