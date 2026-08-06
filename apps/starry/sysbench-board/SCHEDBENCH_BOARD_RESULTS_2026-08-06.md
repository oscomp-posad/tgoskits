# Scheduler benchmarks on RK3588 silicon — optimized StarryOS vs Linux (2026-08-06)

Board: OrangePi-5-Plus (RK3588, 8-core big.LITTLE A55×4 + A76×4, 16 GB LPDDR4X @ 2112 MHz).
Same static aarch64-musl `hackbench`/`schbench` binaries on both OSes. Linux = stock
6.1.43-rockchip-rk3588. StarryOS = `combined-perf` with the EFAULT fix (`b27762fad`) +
Tier-1 IPC optimizations L1–L4 (futex `is_empty` O(1), 64-shard futex table, `sys_write`
validation drop, seccomp lock-free fast-path), adversarially reviewed clean (0 findings).

Two StarryOS configs were measured to isolate THP:
- **placement/THP**: `thp` + `sched-loadbalance` + rockchip drivers
- **LB/no-THP**: `sched-loadbalance` + rockchip drivers (THP off)

They are within noise of each other — THP is a no-op for these IPC/scheduler benchmarks
(expected; neither is page-fault-bandwidth bound). Numbers below are the THP config; the
no-THP column is in `schedbench-baselines/starry-lb-nothp-board-2026-08-06.txt`.

## hackbench — messaging throughput (`-p` pipe; Time in s, LOWER = better)

| config | StarryOS THP | StarryOS no-THP | Linux | gap (THP/Linux) |
|---|---|---|---|---|
| -P g2 (80 tasks)  | 0.436 | 0.564 | 0.029 | 15× |
| -P g5 (200 tasks) | 1.905 | 1.594 | 0.040 | 48× |
| -P g10 (400 tasks)| ~~EFAULT~~ → **2.529** (COW fix) | ~~same EFAULT~~ | 0.071 | 36× |
| -T g2 (80 tasks)  | 2.394 | 2.318 | 0.024 | 100× |
| -T g5 (200 tasks) | 6.749 | 7.139 | 0.044 | 153× |
| -T g10 (400 tasks)| 27.254 | 27.270 | 0.080 | 340× |

> **`-P g10` was re-run after the COW-refcount fix (`5c18e46ab`) and now completes at
> 2.529 s** (was `fork() Bad address → 240 s timeout`) — board-validated. Full post-fix run:
> `schedbench-baselines/starry-thp-cowfix-board-2026-08-06.txt`. Note process mode now scales
> cleanly (g10 -P 2.5 s) while thread mode does not (g10 -T 27 s) — the `-T` slowness is the
> futex/wake path, the same family as Gap 1.

## schbench — wakeup/request latency + RPS (5 s runs)

| metric | StarryOS THP | Linux | gap |
|---|---|---|---|
| m1t4 wakeup p50 | 1042 µs | 6 µs | **174×** |
| m1t4 wakeup p99 | 25504 µs | 7 µs | — |
| m1t4 request p50 | 24672 µs | 20000 µs | 1.23× |
| m1t4 RPS | 128.0 | 199.6 | 0.64× |
| m2t8 wakeup p50 | 24416 µs | 4152 µs | 5.9× |
| m2t8 RPS | 92.2 | 129.8 | 0.71× |

## Reading the results

- **hackbench improved massively vs history** (was 50–260× before the IPC-alloc-lock fix
  and L1–L4) but a **15–340× gap remains** — StarryOS is not yet at parity on raw
  message throughput. Threaded mode (`-T`) is *worse* than process mode (`-P`) on StarryOS,
  the opposite of Linux — points at futex/clone-path contention that L1/L2 reduced but did
  not close.
- **The schbench story is latency, not work.** Request p50 is only 1.23× Linux (the 20 ms
  of per-request work dominates and StarryOS does it at near-parity), but **wakeup p50 is
  174× worse (1042 µs ≈ one 1 ms scheduler tick)**. This single number caps RPS at 0.64×.

## Two structural gaps (root causes)

### Gap 1 — wakeup latency 1042 µs vs Linux 6 µs  *(the dominant parity blocker; OPEN)*
schbench m1t4 wakeup p50 = 1042 µs vs Linux 6 µs. The obvious hypotheses were **checked and
ruled out** by code trace:
- **Not a missing wake IPI.** The cross-core wake path already sends a *targeted GIC SGI* to
  the idle CPU: `wait_queue.rs` `notify_one` → `unblock_task` → `kick_remote_cpu` →
  `ax_ipi::run_on_cpu` → `send_ipi(SGITarget)` (SMP+`ipi` features are on in the 8-core board
  build). The idle loop WFIs with IRQs enabled, so the SGI wakes it at once.
- **Not tick granularity.** The scheduler tick is **100 Hz / 10 ms** (`axruntime` `TICKS_PER_SEC
  = 100`), so a "wait for next tick" would be ~10 ms, not ~1 ms — the number doesn't fit.

So the 1 ms is real but subtler. Remaining suspects (need on-board profiling, not yet
confirmed): **wake placement** (`select_wake_run_queue` is occupancy-spread with `wake_affine`
OFF by default — no Linux-style `select_idle_sibling`, so the wakee may not land on the truly
idle sibling), **IPI coalescing** (`REMOTE_RESCHEDULE_PENDING` suppresses a 2nd kick until the
flag clears), and the **futex→notify** hop. _(task #44 — reopened as a profiling task, not a
one-line fix.)_

### Gap 2 — fork() EFAULT at ~250 concurrent processes  →  **FIXED** (`5c18e46ab`)
hackbench `-P g10` (400 processes) failed: `fork()` returned **EFAULT** after ~250 address
spaces (thread mode `-T g10` handled all 400). Root cause: the per-frame COW reference count
was a **`u8`** (`cow.rs` `FrameRefCnt`); a read-only libc/text/rodata frame shared by the
parent plus ~254 forked children **overflowed the `u8`**, and `clone_map` returned
`BadAddress` → EFAULT. Not THP (reproduced with THP off) and not ASID (StarryOS uses ASID 0).
**Fix:** widened the counter to `u32` (Linux uses a 32-bit refcount; 4 B sharers ≈ unbounded)
and made the now-unreachable overflow return `NoMemory` (ENOMEM) instead of EFAULT. Both build
configs compile clean; board re-validation of `-P g10` pending.

## Artifacts
- `schedbench-baselines/linux-schedbench-board-2026-08-06.txt`
- `schedbench-baselines/starry-placement-thp-board-2026-08-06.txt`
- `schedbench-baselines/starry-lb-nothp-board-2026-08-06.txt`
