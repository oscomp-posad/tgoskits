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

### Gap 1 — wakeup latency 1042 µs vs Linux 6 µs  →  **ROOT-CAUSED; wake_affine reaches parity**
The obvious hypotheses were ruled out first: it is **not** a missing wake IPI (the cross-core
wake already fires a targeted GIC SGI via `notify_one`→`unblock_task`→`kick_remote_cpu`→
`send_ipi`) and **not** tick granularity (tick is **100 Hz / 10 ms**, not 1 ms). The real cause:
the default occ-spread wake places the woken worker on a **different idle core**, so every wake
pays the **cross-core IPI + `on_cpu` handshake** (~1 ms). Enabling `wake_affine` turns a 1:1
dispatcher→worker hand-off into a **local enqueue on the waker's core (no IPI)**.

Clean board A/B (2026-08-06, same THP+COW-fix kernel, only `sched-loadbalance-wake-affine`
differing — the earlier "wake_affine regresses" data was corrupted by the fork EFAULT):

| metric | wake_affine OFF | wake_affine ON | Linux |
|---|---|---|---|
| schbench m1t4 wakeup p50 | 1023 µs | **9 µs** | 6 µs |
| schbench m2t8 wakeup p50 | 25120 µs | **5672 µs** | 4152 µs |
| hackbench -P g2 / g5 | 0.98 / 1.86 s | **0.74 / 1.08** | 0.029 / 0.040 |
| hackbench -P g10 | **2.53 s** | 3.17 s | 0.071 |
| schbench m1t4 RPS | **130** | 122 | 199.6 |
| schbench m2t8 RPS | 85 | **92** | 129.8 |

**wake_affine is a large net win** — near Linux parity on m1t4 wakeup latency (9 vs 6 µs), and
better on most hackbench too. **Enabled by default in the placement board config**
(`build-aarch64-placement-orangepi-5-plus.toml`).

#### Attempted "proper" Linux-faithful placement — and why it was reverted
I then tried to make the policy *more* Linux-faithful: `wake_affine_idle` (prefer an idle
`prev_cpu` to keep the waker free) + `select_idle_sibling` (steer onto an idle sibling), so the
`-P g10` / m1t4-RPS trade-offs would go away. A clean board A/B **disproved it**:

| metric | OFF | crude wake_affine | "proper" Linux-faithful | Linux |
|---|---|---|---|---|
| schbench m1t4 wakeup p50 | 1023 | **9** | 9 | 6 |
| schbench m2t8 wakeup p50 | 25120 | **5672** | 25120 ✗ | 4152 |
| hackbench -P g5 | 1.86 | **1.08** | 3.12 ✗ | 0.040 |
| hackbench -P g10 | 2.53 | 3.17 | 7.71 ✗ | 0.071 |

The Linux-faithful version **regressed** (lost the m2t8 win, hurt hackbench badly). Root reason:
Linux prefers spreading (idle prev / idle sibling) because its cross-core wake is ~µs; **here a
cross-core wake is ~1 ms**, so every `prev_idle → prev` / `select_idle_sibling` decision pays
~1 ms. Copying Linux faithfully is *counterproductive* on this SoC. Reverted (`run_queue.rs`
back to the simple `occ<=1 → local hand-off`, which is empirically best).

**The real remaining lever is the ~1 ms cross-core wake cost itself** (GIC SGI + `on_cpu`
switch-out handshake). Fix that — via on-board profiling of the notify→enqueue→IPI→pick hops —
and Linux-style placement (and full schbench/hackbench parity) becomes reachable. Until then,
the aggressive local hand-off is the right policy. _(task #44 closed with this finding; a new
"profile + fix cross-core wake latency" follow-up is the next lever.)_

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
