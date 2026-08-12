# Syscall-entry cost — board profile + roadmap (2026-08-09)

Board bisect of getpid (RK3588, syscost 500000, placement config unless noted).
All numbers same-session; cross-build deltas carry some thermal noise.

## Where the ~850 ns goes

| variant | getpid ns | note |
|---|---|---|
| baseline (start of turn) | 1026.5 | after SMP-safe SpinNoIrq |
| **+ skip poll_process_timer** | **853.3** | **SHIPPED (commit c0484b23a), −173 ns** |
| bisect: set_timer_state no-op'd | 457.9 | the floor: `uctx.run()` trap + dispatch |
| tickacct coarse (drop boundary accounting, keep lock) | 793.3 | −60 ns only; rusage cpu/wall=1.00 PASS |
| Linux (A76 reference) | 152 | trap + minimal dispatch |

Decomposition:
- **`uctx.run()` trap round-trip + `handle_syscall` dispatch ≈ 458 ns** — the
  floor. Arch-level (SVC → GP save → dispatch → restore → eret). Linux's 152 ns
  is essentially this done leaner; hard to close from the Rust layer.
- **`set_timer_state` ×2 ≈ 335–395 ns** — per-syscall CPU-time accounting at the
  User↔Kernel boundaries. THE remaining lever.
- **`poll_process_timer` ≈ 173 ns** — FIXED (was a redundant global
  PROCESS_TABLE lock + posix-timer lock on every return; gated on a lock-free
  `count` now).

## The `set_timer_state` lever — it's the LOCK, not the accounting

Dropping the accounting arithmetic (coarse tick-only, `tickacct`) saved only
**−60 ns** (853→793) and the accounting stayed *correct* (rusage_acct
cpu/wall=1.00, coarse split s_frac 0.62 vs Linux 0.72, PASS). So the CPU-time
math is cheap (~60 ns). The other **~275–335 ns** is the SMP-safety
`SpinNoIrq<TimeManager>` lock + `try_as_thread` taken twice per syscall.

Linux does NOT account CPU time at syscall boundaries — it samples user/kernel
mode on the timer tick (`TICK_CPU_ACCOUNTING`). StarryOS's per-boundary
lock+account is exactly why its syscalls run ~3× the trap floor.

## Tier 2 — DONE + validated (2026-08-10): lock-free state, getpid −42%

Implemented + board-validated + adversarially reviewed (0 bugs). Moved the
User/Kernel `state` out of the locked `TimeManager` into a lock-free
`Thread::timer_state` (AtomicU8); `tick()`/`poll()` take state as a param; the
tickacct common syscall boundary is now a single Relaxed store (no lock, no
accounting); accounting is purely `on_tick`+`on_leave` sampling the atomic; an
armed interval timer (lock-free `itimer_armed` hint) still takes the lock+poll.

| getpid ns | exact (non-tickacct) | coarse (tickacct, Tier 2) |
|---|---|---|
| this build | 974 | **495.7** |
| vs 853 baseline | — | **−42%, ~at the 458 ns trap floor** |

rusage_acct: coarse cpu/wall=1.00, syscall **s_frac 0.71 ≈ Linux 0.72** (vs the
exact path's 0.62) — the coarse split is now *Linux-like*, total CPU time exact.
Enabled in the ship (placement) config; the non-tickacct path keeps exact
per-boundary accounting as a fallback. Commit `2de99d499`.

getpid is now **~495 ns vs Linux 152 (~3.3×)**, down from ~5.6×. The remainder is
the `uctx.run()` trap round-trip — arch-level.

### (superseded) original Tier-2 estimate

To remove the lock from the common syscall boundary, move the User/Kernel
`state` out of the locked `TimeManager` into a lock-free `AtomicU8` on `Thread`
(plus a lock-free `itimer_armed` hint so the boundary can decide common-vs-armed
without locking). Then:
- common boundary → `thr.timer_state.store(state, Relaxed)` (no lock, no account);
- accounting → purely `on_tick` (every CPU) + `on_leave` (switch), reading the
  atomic state (coarse, Linux-like split — validated above to keep cpu/wall=1.00);
- armed itimer → still lock + `poll()` (rare), so ITIMER_VIRTUAL/PROF stay serviced.

**This is a substantial correctness-critical refactor AND a semantic decision**
(the u/s split becomes tick-granular/coarse like Linux, vs StarryOS's current
exact split). Risk is bounded — the atomic is only a sampling hint; total
accounting stays on the SMP-safe locked ticks/switches — but it changes
observable `getrusage`/`times` precision, so it wants sign-off on:
1. coarse tick-only accounting as the **default** (biggest win, ship config gets
   fast syscalls) vs **gated** behind `tickacct` (default stays exact/slow);
2. is the ~300 ns worth the coarse split for this project.

Smaller alternative levers (keep exact): a preempt-only spinlock for `thr.time`
(drop the IRQ-disable; same-CPU reentrancy already handled by `try_lock`-skip)
saves ~40–80 ns of the lock; the trap floor itself needs arch work.

## Shipped this pass
- `poll_process_timer` fast path — getpid 1026.5 → 853.3 ns (−17%), both configs,
  commit c0484b23a. Raw logs `/tmp/ppt-off.log`, `/tmp/coarse-on.log`,
  `/tmp/bisect-sts.log`.
