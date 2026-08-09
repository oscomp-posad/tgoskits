# StarryOS → Linux parity on RK3588 — the full journey

Target hardware: **OrangePi-5-Plus (RK3588)** — 8-core big.LITTLE (4×A55 "little" cpu0–3, 4×A76
"big" cpu4–7), LPDDR4X, GICv3. Reference OS: stock **Linux 6.1.43-rockchip**. All StarryOS numbers
are the `combined-perf` branch (unpushed local work). Benchmarks (same binaries on both OSes):
**sysbench** (compute/memory), **hackbench** (`-p -g{2,5,10}` `-P`/`-T` — pipe IPC + scheduler),
**schbench** (`-m{1,2} -t{4,8}` — wakeup latency + RPS).

This is the narrative of how StarryOS went from qualitatively-broken-vs-Linux to
structurally-Linux-like on the scheduler/memory/IPC axes, what remains, and every root cause found
along the way.

---

## 0. The measurement rig (and why it kept fighting us)

Everything is gated on being able to run StarryOS on real silicon repeatably.

- **Boot path:** ostool catches the board's U-Boot over serial and uploads a FIT (kernel + dtb).
  Originally via **ymodem @ 1.5 Mbaud** (~90 s, corruption-prone).
- **The board loop's two recurring failures:** (1) StarryOS `reboot` (busybox, no init) is a **no-op**
  on this board — it strands StarryOS at its shell with Ethernet down, unreachable; (2) the host's
  link-local NIC (`en5`, `169.254.99.39`) drops its IP whenever the board's Ethernet link goes down.
- **The fix (this session):** `sdboot-run.py` — TFTP is impossible (the board's U-Boot has **no
  working Ethernet**: RTL8125 has no U-Boot driver, `net list` is empty), but Linux Ethernet + U-Boot
  ext4-read (`mmc 1:2`) + on-board `mkimage` do work. So we `scp` the kernel to the writable ext4
  home (sudo-free), build the FIT on the board, and `load mmc; bootm` locally (~1 s vs ~90 s). Plus
  `reboot -f` everywhere for reliable return-to-Linux. See `SDBOOT.md`.

---

## 1. CPU frequency / DVFS parity (the "are the cores even running as fast?" axis)

Before comparing scheduler behavior, the cores must run at Linux clocks.

- **Root cause of undervolt/overshoot:** RK3588 CPU clock is **voltage-coupled via PVTPLL**, and the
  CPU CRU is BL31-secure — StarryOS setting an SCMI rate (e.g. 1200) overshot to ~1470 because Linux
  actually pins the exact rate by setting **PMIC voltage per-OPP**.
- **The lever found:** the **PVTPLL "ring-length"** — rings 1416/1608 give more MHz/volt. Board table
  A76→2256 MHz (ring1608@1000 mV), A55→1881 (ring1608@950), reaching **per-core ≈ Linux**.
- **DDR:** the DMC frequency is the Rockchip SIP DRAM interface (SMC `0x82000008` + shared page), NOT
  SCMI clock. A DDR/DMC ramp-to-2112 MHz driver (voltage-first precondition) closed the
  memory-bandwidth gap.
- **Guardrails:** an all-core brownout audit caps the A76 top rung until 8-thread @ 1.0 V is
  validated (the PSU browns out at 8 cores under load).

Outcome: **CPU/DVFS at parity** — the compute axis is not the bottleneck.

---

## 2. Memory — THP-lite (and it beats Linux)

- **What:** transparent 2 MiB huge pages for private anonymous memory — a `split_huge` primitive
  (2 MiB→512×4 KiB), THP promotion in the anon `mmap` path, graceful 4 KiB fallback under
  fragmentation, and fork/COW at 2 MiB granularity. Feature-gated (`thp`, off by default).
- **Outcome:** on sysbench memory, **THP-StarryOS beats Linux**; per-core sysbench is at parity and
  single-thread is a win. So the **compute + memory axes reach or exceed Linux**.

---

## 3. Scheduler — big.LITTLE placement + the wake-latency saga

This is where StarryOS was qualitatively unlike Linux, and where most of the depth is.

- **Capacity-aware placement (big.LITTLE):** per-CPU capacity table from the DTS, per-run-queue
  runnable count, capacity-aware initial + wake placement — **placement-only, no continuous
  migration** (the full idle-pull/push LB *regressed* sysbench on this SoC via migration thrash;
  the safe placement-only subset was the keeper). Feature `sched-loadbalance`.
- **The single-atom occupancy counter (`occ`):** multi-thread spread was breaking on a two-counter
  race; unified to one saturating atomic with a per-tick self-heal.
- **Wake latency 1042 µs → near-parity — the multi-layer root cause.** schbench m1t4 wakeup p50 was
  **1042 µs** vs Linux 6 µs. The hunt:
  1. First lever: **`wake_affine`** (occ≤1 → hand the wakee to the waker's CPU, local enqueue, no
     IPI). Board A/B (after the EFAULT fix below) decisive: **1023 → 9 µs** (Linux 6 µs). A
     *more* Linux-faithful variant (prefer-idle-prev + select_idle_sibling) **regressed** it —
     because this SoC's cross-core wake is ~1 ms, which **inverts** Linux's cheap-cross-core cost
     model. Aggressive local hand-off is correct here.
  2. But *why* is a cross-core wake ~1 ms even onto a genuinely idle CPU? Built **wakeprof**
     (`/proc/wakeprof`: per-category wake-to-run histograms + hop timers). It proved the ~1 ms is the
     **IPI→idle-pick mechanism**, not queueing/tick/deferral. Hop-split: `ipi_deliver` p50 ~1 ms,
     `pick_after_handler` p50 2 µs.
  3. **Definitive root cause:** the reschedule **GIC SGI does not promptly wake a WFI-halted CPU on
     RK3588** (87 % of reschedule SGIs target a genuinely WFI CPU yet take ~1 ms; the CPU actually
     advances on the ~1–2 ms oneshot timer). GIC enable/routing/target-list all verified correct in
     source — it's a hardware/GIC property, not a config bug.
  4. **Fix:** **poll-idle** (Linux `poll_idle`, feature `idle-poll`, off by default) — spin-check the
     runqueue ~50 µs before deep WFI, catching cross-core-enqueued tasks without depending on the SGI
     waking WFI. Board: xcore-idle wake p50 **1048 → 4 µs (~250×)**. Kept off-by-default (spin =
     power/throughput cost); `wake_affine` covers the latency-critical 1:1 case for free.
- **A negative result worth keeping:** the adaptive haltpoll window regressed (the halt-duration
  signal is confounded by the SGI-doesn't-wake-WFI quirk) — fixed 50 µs is best.

---

## 4. IPC / syscall throughput — the frontier

hackbench (pipe ping-pong) stressed the syscall + wake + context-switch path and exposed several
StarryOS-specific pathologies, each since fixed:

- **Tier-1 IPC:** futex table **64-way sharded** + O(1) empty check, killed a per-message bounce
  `Vec`, on-stack wakers, dropped a spurious `yield_now`, seccomp fast-path.
- **Alloc-free wake:** `PollSet::wake` made allocation-free + per-task cached `AxWaker` (pipe wakeup
  was allocating 4–6×/message under a global TLSF lock — a 50–260× hackbench pathology, root-caused
  to the **global allocator lock**, not placement).
- **fork-at-scale EFAULT (fixed, commit `5c18e46ab`):** at ~250 forked children `fork()` returned
  EFAULT. NOT THP, NOT ASID — the per-frame **COW refcount was a `u8`** and overflowed at 255 sharers
  (a shared libc/text page). Widened to `u32` + ENOMEM on overflow. Board: hackbench `-P g10` (400
  procs) went from EFAULT→timeout to **2.5 s**.
- **The `-T` serialization — this session's headline (commits `52569c749`, `228706d2c`).** Threaded
  (`-T`, `CLONE_VM`) hackbench was **3–10× slower than `-P`** (fork) — the *opposite* of Linux.
  Root cause: every `sys_read`/`sys_write` byte-copy and every `UserPtr` validation takes the
  **shared, exclusive, sleeping `Arc<Mutex<AddrSpace>>`**; CLONE_VM threads share one aspace and
  fully serialize, forks have private ones.
  - **Chosen fix:** NOT the literal `RwLock<AddrSpace>` (no sleeping rwlock exists in-tree; building
    one + converting 94 lock sites buys no perf over the alternative). Instead the **Linux `access_ok`
    model**: a lock-free HW page-table probe (aarch64 `AT S1E0R/W` + `PAR_EL1`) on the user-copy hot
    path — present pages skip the aspace lock entirely; misses fall to the unchanged slow path.
    Feature `user-access-fastpath`.
  - **Security:** a 5-lens adversarial review + verify pass caught a real **HIGH** bug (unchecked
    arithmetic let a wrapping `UserPtr` skip the probe loop → kernel-half deref); fixed with checked
    arithmetic, then a dedicated re-verify confirmed **no bypass**. AT-permitted ⟹ the user could do
    the access itself ⟹ no privilege escalation.
  - **Board A/B (same HEAD, only the feature differing):** `-T g10` **28.5 → 2.78 s (10.2×)**,
    `-T g5` **6.5 → 1.05 s (6.25×)**; `-T/-P` ratio **3.6–4.2× → ~1** (Linux-like); `-P` also
    1.5–2× better. **Promoted into the ship config.** Two lower-risk attempts were empirically ruled
    out first (adaptive-poll; demand-fault-copy — the latter proved the bottleneck is *concurrency*,
    not hold-time).

---

## 5. Where we stand vs Linux (2026-08-09, ship config)

| hackbench (Time s, lower=better) | Linux | StarryOS | gap |
|---|---|---|---|
| -P g2 / g5 / g10 | 0.029 / 0.040 / 0.071 | 1.37 / 0.90 / 3.96 | ~23–56× |
| -T g2 / g5 / g10 | 0.024 / 0.044 / 0.080 | 1.63 / 1.05 / 2.78 | ~24–35× |

| schbench | Linux | StarryOS | gap |
|---|---|---|---|
| m1t4 wakeup p50 | 6 µs | 14 µs | ~2.3× |
| m2t8 wakeup p50 | 4152 µs | 8688 µs | ~2.1× |
| m1t4 / m2t8 RPS | 199.6 / 129.8 | 94 / 91 | 0.47× / 0.70× |

**Read honestly:**
- **Compute + memory (sysbench): at parity / winning.**
- **Scheduler *structure*: Linux-like.** No fork-EFAULT at scale; `-T ≈ -P` (the inversion is gone);
  wakeup latency within ~2× of Linux.
- **Absolute IPC throughput (hackbench): still ~20–60× off**, and now *uniform* across `-P`/`-T` — so
  it is no longer a lock/placement pathology but the **fundamental per-message cost** (syscall
  entry/exit + pipe buffer copy + wake + context switch). That is the remaining frontier.

---

## 6. Lessons (the counterintuitive ones)

- **Being more Linux-faithful can be worse here.** The prefer-idle-prev/select-idle-sibling wake and
  the RwLock refactor were both "more like Linux" and both wrong for this SoC — because RK3588's
  ~1 ms cross-core wake inverts Linux's cost assumptions, and because no sleeping rwlock exists.
- **Root-cause before optimizing.** EFAULT looked like THP/ASID (was a `u8` overflow); the wake gap
  looked like a missing IPI/1 kHz tick (was the SGI not waking WFI); the `-T` gap looked like it
  needed an RwLock (was solved lock-free). Each wrong first guess cost a board cycle; the profilers
  (`wakeprof`, hop-split, in-WFI counter) paid for themselves.
- **Adversarially verify before shipping a security-boundary change.** The `-T` fix's real HIGH bug
  was found by an independent verify pass, not the author.
- **Negative results are results.** Adaptive-poll, demand-fault-copy, full idle-pull LB, the
  Linux-faithful wake — all board-tested, all reverted, all documented so they aren't re-tried.

---

## 7. What's next

1. **Attack the absolute hackbench gap** (the per-message IPC/context-switch/wake cost) — the genuine
   remaining Linux-parity lever. Starts with a profiling pass: where does one pipe ping-pong spend
   its ~20–60×? `idle-poll` is *not* it (helps schbench latency, ~20 % worse on hackbench).
2. **schbench RPS (0.5–0.7×)** — throughput under load, same wake/ctxsw path; likely falls out of #1.

---

## 8. Artifacts

- **Branch:** `combined-perf` (local, unpushed). Key commits: COW-EFAULT `5c18e46ab`; wake_affine
  `65db277f4`; `-T` fast path `52569c749` + board-validation/promotion `228706d2c`; SD-boot
  `27662ad49`.
- **Ship config:** `build-aarch64-placement-orangepi-5-plus.toml` (thp + sched-loadbalance +
  wake_affine + user-access-fastpath).
- **Design/results docs (this dir):** `ACCESS_FASTPATH_DESIGN_2026-08-08.md`,
  `THREADED_HACKBENCH_ROOTCAUSE_2026-08-07.md`, `SCHEDBENCH_BOARD_RESULTS_2026-08-06.md`,
  `SDBOOT.md`; raw serial captures under `schedbench-baselines/`.
- **Diagnostics (feature-gated, off by default):** `wakeprof` (`/proc/wakeprof`), `idle-poll`.
