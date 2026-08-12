//! Time management module.

use alloc::{borrow::ToOwned, collections::binary_heap::BinaryHeap, sync::Arc};
use core::{mem, time::Duration};

use ax_kspin::SpinNoIrq as Mutex;
use ax_runtime::hal::time::{NANOS_PER_SEC, TimeValue, monotonic_time_nanos, wall_time};
use ax_task::{
    WeakAxTaskRef, current,
    future::{block_on, timeout_at_wall},
};
use event_listener::{Event, listener};
use spin::LazyLock;
use starry_process::Pid;
use starry_signal::Signo;
use strum::FromRepr;

use crate::task::{poll_process_timer, poll_timer};

fn time_value_from_nanos(nanos: usize) -> TimeValue {
    let secs = nanos as u64 / NANOS_PER_SEC;
    let nsecs = nanos as u64 - secs * NANOS_PER_SEC;
    TimeValue::new(secs, nsecs as u32)
}

#[derive(Debug, Clone)]
pub enum AlarmTarget {
    Thread(WeakAxTaskRef),
    Process(Pid),
}

struct Entry {
    deadline: Duration,
    target: AlarmTarget,
}

impl PartialEq for Entry {
    fn eq(&self, other: &Self) -> bool {
        self.deadline == other.deadline
    }
}
impl Eq for Entry {}
impl PartialOrd for Entry {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Entry {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        other.deadline.cmp(&self.deadline)
    }
}

static ALARM_LIST: LazyLock<Mutex<BinaryHeap<Entry>>> =
    LazyLock::new(|| Mutex::new(BinaryHeap::new()));
static EVENT_NEW_TIMER: LazyLock<Event> = LazyLock::new(Event::new);

/// The type of interval timer.
#[repr(i32)]
#[allow(non_camel_case_types)]
#[derive(Eq, PartialEq, Debug, Clone, Copy, FromRepr)]
pub enum ITimerType {
    /// Real elapsed wall-clock time.
    Real    = 0,
    /// User-mode CPU time.
    Virtual = 1,
    /// User + kernel CPU time.
    Prof    = 2,
}

impl ITimerType {
    /// Returns the signal number associated with this timer type.
    pub fn signo(&self) -> Signo {
        match self {
            ITimerType::Real => Signo::SIGALRM,
            ITimerType::Virtual => Signo::SIGVTALRM,
            ITimerType::Prof => Signo::SIGPROF,
        }
    }
}

#[derive(Default)]
struct ITimer {
    interval_ns: usize,
    remained_ns: usize,
}

impl ITimer {
    pub fn new(interval_ns: usize, remained_ns: usize) -> Self {
        let result = Self {
            interval_ns,
            remained_ns,
        };
        result.renew_timer();
        result
    }

    pub fn update(&mut self, delta: usize) -> bool {
        if self.remained_ns == 0 {
            return false;
        }
        if self.remained_ns > delta {
            self.remained_ns -= delta;
            false
        } else {
            self.remained_ns = self.interval_ns;
            self.renew_timer();
            true
        }
    }

    pub fn renew_timer(&self) {
        if self.remained_ns > 0 {
            let deadline = wall_time() + Duration::from_nanos(self.remained_ns as u64);
            register_alarm(deadline);
        }
    }
}

/// Register an alarm at the given wall-clock deadline for the current task.
/// Used by both ITimer and POSIX timers.
pub fn register_alarm(deadline: Duration) {
    register_alarm_for(deadline, AlarmTarget::Thread(Arc::downgrade(&current())));
}

/// Register an alarm at the given wall-clock deadline for a specific target.
/// Used when re-arming periodic POSIX timers from the alarm_task context,
/// where `current()` is the alarm_task, not the user task.
pub fn register_alarm_for(deadline: Duration, target: AlarmTarget) {
    let mut guard = ALARM_LIST.lock();
    let should_wake = guard.peek().is_none_or(|it| it.deadline > deadline);
    guard.push(Entry { deadline, target });
    drop(guard);
    if should_wake {
        EVENT_NEW_TIMER.notify(1);
    }
}

/// Represents the state of the timer.
///
/// Stored lock-free in `Thread::timer_state` (as `u8`) so the syscall boundary
/// can flip User/Kernel without taking the `time` lock; the tick/switch
/// accounting reads it back via [`TimerState::from_u8`].
#[repr(u8)]
#[derive(Debug, Clone, Copy)]
pub enum TimerState {
    /// Fallback state.
    None   = 0,
    /// The timer is running in user space.
    User   = 1,
    /// The timer is running in kernel space.
    Kernel = 2,
}

impl TimerState {
    /// Decodes the discriminant stored in the `Thread::timer_state` atomic.
    /// Any unknown value maps to `None` (accounts nothing).
    pub fn from_u8(v: u8) -> Self {
        match v {
            1 => TimerState::User,
            2 => TimerState::Kernel,
            _ => TimerState::None,
        }
    }
}

/// A manager for time-related operations.
pub struct TimeManager {
    utime_ns: usize,
    stime_ns: usize,
    /// Baseline for itimer delta calculation in `poll()`.
    /// Updated only by `poll()`, never by `tick()`.
    last_wall_ns: usize,
    /// Baseline for tick-based CPU time accumulation.
    /// Updated by `tick()` and synced to `last_wall_ns` at the end of `poll()`.
    last_tick_ns: usize,
    itimers: [ITimer; 3],
}

impl Default for TimeManager {
    fn default() -> Self {
        Self::new()
    }
}

impl TimeManager {
    pub(crate) fn new() -> Self {
        Self {
            utime_ns: 0,
            stime_ns: 0,
            last_wall_ns: 0,
            last_tick_ns: 0,
            itimers: Default::default(),
        }
    }

    /// Returns the current user time and system time as a tuple of `TimeValue`.
    pub fn output(&self) -> (TimeValue, TimeValue) {
        let utime = time_value_from_nanos(self.utime_ns);
        let stime = time_value_from_nanos(self.stime_ns);
        (utime, stime)
    }

    /// Accumulates CPU time for the current tick without emitting signals.
    ///
    /// Safe to call from IRQ/timer-callback context.  Signal-bearing itimers
    /// are checked only through the full `poll()` path at syscall boundaries.
    ///
    /// Uses `last_tick_ns` as the exclusive baseline so that `poll()`'s
    /// itimer accounting (which uses the independent `last_wall_ns`) is not
    /// affected.
    ///
    /// `state` is the User/Kernel mode to attribute the elapsed slice to,
    /// supplied by the caller from the lock-free `Thread::timer_state` atomic.
    ///
    /// `resume_floor_ns` is the thread's last resume instant
    /// ([`Thread::resume_floor_ns`](crate::task::Thread::resume_floor_ns)); the
    /// billed baseline is clamped to at least it so a slice that began before the
    /// thread was descheduled does not charge the descheduled gap to utime/stime.
    /// Callers without that floor (exact accounting) pass `0`, a no-op.
    pub fn tick(&mut self, state: TimerState, resume_floor_ns: usize) {
        let now_ns = monotonic_time_nanos() as usize;
        let delta = now_ns.saturating_sub(self.last_tick_ns.max(resume_floor_ns));
        match state {
            TimerState::User => self.utime_ns += delta,
            TimerState::Kernel => self.stime_ns += delta,
            TimerState::None => {}
        }
        self.last_tick_ns = now_ns;
        // last_wall_ns is intentionally NOT touched here so that poll()
        // continues to see the full wall-clock delta for itimer accounting.
    }

    /// Polls the time manager to update CPU time and interval timers, returning
    /// the interval-timer signals that fired (at most 3, in slot order
    /// Virtual/Prof/Real).
    ///
    /// The caller MUST emit the returned signals AFTER releasing the `time`
    /// lock: signal delivery takes other locks, and the `time` lock is
    /// IRQ-disabling — running the emitter under it would extend the IRQs-off
    /// window and risk a lock-ordering deadlock. Returning the signals keeps
    /// the locked region free of any nested lock.
    ///
    /// `resume_floor_ns` clamps the utime/stime baseline exactly as in
    /// [`tick`](Self::tick) (interval-timer accounting, which tracks wall time,
    /// is unaffected). Callers without that floor pass `0`, a no-op.
    #[must_use = "the returned itimer signals must be emitted after unlocking"]
    pub fn poll(&mut self, state: TimerState, resume_floor_ns: usize) -> [Option<Signo>; 3] {
        let now_ns = monotonic_time_nanos() as usize;
        // itimer_delta: full wall-clock time since the last poll() call.
        // Used for interval-timer accounting so they fire at the right time
        // regardless of whether tick() has been called in between.
        let itimer_delta = now_ns.saturating_sub(self.last_wall_ns);
        // remaining: time since the last tick() that has not yet been counted
        // in utime_ns / stime_ns, floored at the resume instant so a descheduled
        // gap is not billed. If tick() was never called and the thread was not
        // descheduled, last_tick_ns == last_wall_ns and remaining == itimer_delta.
        let remaining = now_ns.saturating_sub(self.last_tick_ns.max(resume_floor_ns));
        // Fixed slots so no `n` counter is needed: 0=Virtual, 1=Prof, 2=Real.
        let mut fired = [None; 3];
        match state {
            TimerState::User => {
                self.utime_ns += remaining;
                if self.itimers[ITimerType::Virtual as usize].update(itimer_delta) {
                    fired[0] = Some(ITimerType::Virtual.signo());
                }
                if self.itimers[ITimerType::Prof as usize].update(itimer_delta) {
                    fired[1] = Some(ITimerType::Prof.signo());
                }
            }
            TimerState::Kernel => {
                self.stime_ns += remaining;
                if self.itimers[ITimerType::Prof as usize].update(itimer_delta) {
                    fired[1] = Some(ITimerType::Prof.signo());
                }
            }
            TimerState::None => {}
        }
        if self.itimers[ITimerType::Real as usize].update(itimer_delta) {
            fired[2] = Some(ITimerType::Real.signo());
        }
        self.last_wall_ns = now_ns;
        // Sync tick baseline with poll baseline so the next tick() starts
        // from a clean slate.
        self.last_tick_ns = now_ns;
        fired
    }

    /// Returns whether any interval timer is currently armed.
    ///
    /// When none is armed, a syscall boundary can skip the full `poll()` (a
    /// clock read + itimer scan + signal emission) and rely on tick/switch
    /// accounting for utime/stime — there is no itimer deadline to service.
    #[cfg(feature = "tickacct")]
    pub fn has_armed_itimer(&self) -> bool {
        self.itimers.iter().any(|it| it.remained_ns > 0)
    }

    /// Sets the interval timer of the specified type with the given interval
    /// and remaining time.
    pub fn set_itimer(
        &mut self,
        ty: ITimerType,
        interval_ns: usize,
        remained_ns: usize,
    ) -> (TimeValue, TimeValue) {
        // Re-baseline the itimer clock on the disarmed->armed transition. Under
        // `tickacct`, `poll()` — the only writer of `last_wall_ns` — is skipped
        // at syscall boundaries while no itimer is armed, so `last_wall_ns` can
        // be stale by seconds (or still 0 from `new()`). Without this reset the
        // first `poll()` after arming would compute `itimer_delta = now -
        // last_wall_ns` as that whole stale span and fire the freshly-armed
        // ITIMER_REAL/PROF immediately. Re-baselining makes the first post-arm
        // delta measure only from the arm point. Guard on `!has_armed_itimer()`
        // (tested before the replace, i.e. the pre-arm state): while any itimer
        // was already armed, poll() ran every boundary and kept last_wall_ns
        // fresh, so re-basing then would drop this syscall's own kernel window
        // from the already-running timer. No-op semantically without `tickacct`
        // (poll runs every boundary there), so gated to keep that path
        // byte-identical.
        #[cfg(feature = "tickacct")]
        if remained_ns > 0 && !self.has_armed_itimer() {
            self.last_wall_ns = monotonic_time_nanos() as usize;
        }
        let old = mem::replace(
            &mut self.itimers[ty as usize],
            ITimer::new(interval_ns, remained_ns),
        );
        (
            time_value_from_nanos(old.interval_ns),
            time_value_from_nanos(old.remained_ns),
        )
    }

    /// Gets the current interval and remaining time.
    pub fn get_itimer(&self, ty: ITimerType) -> (TimeValue, TimeValue) {
        let itimer = &self.itimers[ty as usize];
        (
            time_value_from_nanos(itimer.interval_ns),
            time_value_from_nanos(itimer.remained_ns),
        )
    }
}

async fn alarm_task() {
    loop {
        let mut guard = ALARM_LIST.lock();
        let Some(entry) = guard.peek() else {
            drop(guard);
            listener!(EVENT_NEW_TIMER => listener);

            if !ALARM_LIST.lock().is_empty() {
                continue;
            }
            listener.await;

            continue;
        };

        let now = wall_time();
        if entry.deadline <= now {
            let entry_deadline = entry.deadline;
            let target = entry.target.clone();
            // pop() runs unconditionally (it removes the peeked entry); only the
            // peek-then-pop invariant is asserted, and only in debug — under the
            // same held lock it is locally provable, so a release build must not
            // be able to panic the alarm subsystem here.
            let popped = guard.pop();
            debug_assert!(popped.is_some_and(|it| it.deadline == entry_deadline));
            drop(guard);
            match target {
                AlarmTarget::Thread(weak_task) => {
                    if let Some(task) = weak_task.upgrade() {
                        poll_timer(&task);
                    }
                }
                AlarmTarget::Process(pid) => {
                    poll_process_timer(pid);
                }
            }
        } else {
            let deadline = entry.deadline;
            drop(guard);
            listener!(EVENT_NEW_TIMER => listener);
            if ALARM_LIST
                .lock()
                .peek()
                .is_none_or(|it| it.deadline != deadline)
            {
                continue;
            }
            let _ = timeout_at_wall(Some(deadline), listener).await;
        }
    }
}

/// Spawns the alarm task.
pub fn spawn_alarm_task() {
    info!("Initialize alarm...");
    ax_task::spawn_raw(
        || block_on(alarm_task()),
        "alarm_task".to_owned(),
        ax_task::default_task_stack_size(),
    );
}
