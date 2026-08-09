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

## Remaining layers (next — task #59 continues)

getpid is still ~900 ns vs Linux 134 ns (~6.7×). Remaining per-syscall contributors to profile/strip:
- `set_timer_state(Kernel)` + `set_timer_state(User)` — **twice** per syscall (kernel/user time
  accounting); check the cost.
- `check_signals` on every syscall return (even with no pending signal).
- the `handle_syscall` dispatch mechanism (match vs jump table).
- `uctx.run()` / `ReturnReason` handling + the SVC→save→dispatch→restore→eret round trip.
- the pipe data path itself (pipe_wr − getpid ≈ 5 µs of pipe-specific work vs Linux ~0.4 µs).

Each is a candidate for a Linux-style fast path. Closing the syscall-entry gap is the highest-value
remaining lever for hackbench (and every other syscall-bound workload) — far more than any single
scheduler tweak.

Artifacts: `ppong.c` (windowed), `syscost.c`, `uboot-ppongbatch-short.toml`,
`uboot-syscost-short.toml`; logs `/tmp/ppongbatch.log`, `/tmp/syscost.log`, `/tmp/syscost-fastpath.log`.
