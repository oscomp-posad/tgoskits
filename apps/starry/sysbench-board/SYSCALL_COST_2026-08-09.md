# The hackbench gap is fundamentally syscall-entry cost (2026-08-09)

Continuing the profile of the StarryOS-vs-Linux hackbench gap after the oversubscription-cliff work
turned out to be variance-dominated. Using a **windowed** ping-pong (`ppong … B`) and a raw
syscall-cost probe (`syscost`) on the board via the fast sdboot loop, the gap decomposes cleanly and
reproducibly — no scheduler variance involved.

## The decomposition

**Windowed ppong** (writer sends B msgs before draining B — B amortizes context switches):

| per-msg (µs) | B=1 | B=4 | B=16 | B=64 |
|---|---|---|---|---|
| Linux    | 12.3 | 4.1 | 2.0 | 1.87 |
| StarryOS | 24   | 15.3 | 13.6 | 13.4 |

Both batch (per-msg falls with B), but StarryOS's **batched floor is 13.4 µs vs Linux 1.87 µs = 7×**.
Batching removes switches, so the floor is *syscall-dominated*.

**Raw syscall cost** (`syscost`, A76):

| | Linux | StarryOS | ratio |
|---|---|---|---|
| getpid (raw entry/exit) | 134 ns | 1200 ns | **9×** |
| pipe write+read | 558 ns | 6445 ns | 11.5× |

**So the fundamental gap is syscall entry/exit: ~9× Linux (1200 ns ≈ 2400 cycles for a trivial
getpid).** It taxes every syscall, and hackbench is syscall-saturated — this, not the scheduler, is
the dominant hackbench-gap contributor. (Per-message *latency* and *wake* were already near-parity;
the trap register save/restore is GP-only, no FP/SIMD — not the cost.)

## Fix 1 (landed) — ptrace fast path on the syscall hot path

`task/user.rs` ran the per-syscall ptrace trace-state machinery **unconditionally**:
`take_ptrace_syscall_trace_for` (= `lock().remove(&tid)` on a `SpinNoIrq<BTreeMap>`) **twice** per
syscall, `has_ptrace_pending_event_for` (`lock().contains_key`), etc. — 3–4 spinlock+BTreeMap ops per
syscall, pure overhead when no debugger is attached.

Fix: gate that block on the already-computed `is_ptraced` (two cheap atomic loads; the loop-top
singlestep block already used this gate) — Linux's `TIF_SYSCALL_WORK` model. Behavior-preserving when
traced.

**Board result: getpid 1200 → ~900 ns (−25%)**, pipe_wr 6445 → 5944 ns. A real cut on *every*
syscall. (Untraced is the overwhelmingly common case; a debugger attaching mid-syscall is handled on
the next boundary, as in Linux.)

## Fix 2 (identified, ~250 ns) — timer accounting is done per-syscall, not per-tick

Bisect: no-oping `set_timer_state` dropped getpid **900 → ~650 ns (−250 ns)** and pipe_wr −500 ns
(2 syscalls). So the per-syscall time accounting is the next-biggest layer.

`set_timer_state` runs `TimeManager::poll()` on **every** syscall boundary (twice: Kernel on entry,
User on exit). `poll()` reads `monotonic_time_nanos()` and splits the elapsed time into utime/stime +
checks the interval timers. `update_itimer`/`ITimer::update` already fast-path unarmed timers, so the
cost is the **clock read + per-transition accounting**, ×2 per syscall.

**Linux does not do this** — it accounts CPU time on the timer **tick** (samples user-vs-kernel state
once per tick), not on every user/kernel transition. StarryOS's per-transition model is more precise
but pays a clock read + poll on every syscall.

**Two follow-up measurements (board):**
- **The clock read is NOT the cost.** Swapping `poll()`'s `monotonic_time_nanos()` for a bare
  `mrs cntpct_el0` moved getpid only ~900→~868 ns (~30 ns). So `monotonic_time_nanos()` is already a
  cheap `CNTPCT` read; the ~250 ns is the **`poll()` accounting work itself**, run twice per syscall
  (RefCell borrow + arithmetic + the 2 field writes + `set_state`), not the timer source.
- **Context switch does NOT account CPU time.** `set_timer_state` is called *only* at the two
  syscall boundaries; `switch_to` has no time accounting. utime/stime are accumulated at (a) syscall
  boundaries via `poll()` and (b) timer ticks via `tick()`.

**So the fix is correctness-sensitive and cross-crate (task #60, scoped, not rushed):** to stop
`poll()`-ing on every syscall, CPU time must instead be accounted **on context switch** (attribute
the outgoing task's time-since-last-tick to its state) — otherwise a task that blocks between ticks
(exactly hackbench's tasks) loses its CPU time and utime/stime under-count. That means adding a
time-accounting hook to `axtask::switch_to` (e.g. via the existing `on_sched_switch` tracepoint) and
then reducing `set_timer_state` to a cheap `set_state` on the syscall boundary (full `poll()` only
when an interval timer is armed). This touches the scheduler hot path and time-accounting semantics,
so it needs its own focused pass with a utime/stime-accuracy test + board A/B — not a tail-of-session
edit.

## Remaining layers after that

getpid would be ~650 ns vs Linux 134 ns (~4.8×). Still to profile/strip:
- `check_signals` on every syscall return (even with no pending signal) — needs a cheap
  pending-mask fast path if it doesn't have one.
- the `handle_syscall` dispatch (match vs jump table) + arg decode.
- `uctx.run()` / `ReturnReason` handling + the SVC→save→dispatch→restore→eret round trip.
- the pipe data path (pipe_wr − getpid ≈ 5 µs of pipe-specific work vs Linux ~0.4 µs) — separate,
  pipe-specific.

Each is a Linux-style fast-path candidate. Closing the syscall-entry gap is the highest-value
remaining parity lever for hackbench and every syscall-bound workload — far more than any scheduler
tweak.

Artifacts: `ppong.c` (windowed), `syscost.c`, `uboot-ppongbatch-short.toml`,
`uboot-syscost-short.toml`; logs `/tmp/ppongbatch.log`, `/tmp/syscost.log`, `/tmp/syscost-fastpath.log`.
