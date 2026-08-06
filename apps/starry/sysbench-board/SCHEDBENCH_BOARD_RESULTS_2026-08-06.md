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
| -P g10 (400 tasks)| **fork() Bad address → timeout** | **same EFAULT** | 0.071 | ✗ |
| -T g2 (80 tasks)  | 2.394 | 2.318 | 0.024 | 100× |
| -T g5 (200 tasks) | 6.749 | 7.139 | 0.044 | 153× |
| -T g10 (400 tasks)| 27.254 | 27.270 | 0.080 | 340× |

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

### Gap 1 — wakeup latency ≈ 1 scheduler tick  *(the dominant parity blocker)*
schbench m1t4 wakeup p50 = 1042 µs ≈ 1 ms = one tick at 1000 Hz. A woken worker placed on
an idle CPU is not run until that CPU's next timer tick, i.e. the cross-core wake does not
kick the idle (WFI) CPU immediately. Linux sends a reschedule IPI. **Fix direction:** send a
reschedule SGI/IPI to the target CPU on cross-core wake so it leaves WFI at once.
_(root-cause code path: see below / task #44)_

### Gap 2 — fork() EFAULT at ~250 concurrent processes  *(scale edge case)*
hackbench `-P g10` (400 processes) fails: `fork()` returns **EFAULT** after ~250 address
spaces. Thread mode (`-T g10`, shared address space) handles all 400. Reproduces WITH and
WITHOUT THP, so it is not the THP-COW path; ASID is ruled out (StarryOS uses ASID 0 with
global TLB flush). Some per-address-space resource is exhausted and the failure is
mis-mapped to EFAULT (Linux would return EAGAIN/ENOMEM). _(root-cause: see below / task #45)_

## Artifacts
- `schedbench-baselines/linux-schedbench-board-2026-08-06.txt`
- `schedbench-baselines/starry-placement-thp-board-2026-08-06.txt`
- `schedbench-baselines/starry-lb-nothp-board-2026-08-06.txt`
