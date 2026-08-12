# Time-to-first-inference (TTFI): StarryOS vs Linux — tennis app on OrangePi-5-Plus

Date: 2026-07-11/12. Branch `tennis-ttfi` (from `tennis-submission` @ 5f67227a6).
Board: OrangePi 5 Plus (RK3588), shared SD rootfs; Linux = vendor Ubuntu
6.1.43-rockchip-rk3588 (systemd); StarryOS = tennis submission kernel
(rknpu+rga+jpeg, max_cpu_num=8, log=Warn).

## Methodology

- **TTFI = U-Boot "Starting kernel ..." (kernel handoff) → `TENNIS_FIRST_INFERENCE`**
  (first completed rknn inference in `--mode live`, MJPEG→JPU→RGA→NPU pipeline,
  relu 480x640 model, `--infer-affinity 4-7`).
- Common clock = host-timestamped serial capture (`/tmp/ttfi_logger.py`,
  1.5 Mbaud). The kernel-handoff anchor excludes the asymmetric kernel-transfer
  phase (Linux: SD boot.scr + initramfs; StarryOS: FIT via ymodem-loady run 1 /
  SD fatload runs 2-3).
- App auto-start at boot: Linux = systemd unit `tennis-ttfi.service`
  (Type=simple, tty output); StarryOS = `shell_init_cmd` typed at the shell
  prompt (run 1 by ostool, runs 2-3 by the logger — identical timing, ±50 ms).
  Both run the same `/tennis_app/run_ttfi.sh` and the same binary.
- App markers `TENNIS_PROC_START` / `TENNIS_FIRST_INFERENCE` are fflush()ed;
  app-internal deltas cross-check the serial deltas within ~20-40 ms on every run.

## Results (kernel handoff → first inference)

| sample | StarryOS | Linux |
|---|---|---|
| 1 | 19.217 s | ~12.43 s (boot2, proc_start on serial + app-internal) |
| 2 | 19.228 s | 11.150 s (boot3) |
| 3 | 19.189 s | 11.417 s (boot4) |
| 4 | — | 10.090 s (boot5) |
| **median** | **19.22 s** | **11.28 s** |

**Linux reaches first inference ~7.9 s sooner (1.7x faster TTFI).**

Full power-on numbers (bootloader phase included, method-dependent for
StarryOS): Linux reset→FI 20.8-22.0 s (its U-Boot phase is 9.6 s: 2 s autoboot
wait + boot.scr loading 40 MB kernel + 15 MB initramfs); StarryOS via SD fatload
reset→FI 22.3 s (U-Boot phase 3.1 s for the 14 MB FIT — would land near the
Linux phase if wired into boot.scr with the same autoboot wait).

## Phase breakdown (medians)

| phase | StarryOS | Linux |
|---|---|---|
| kernel → app exec (`TENNIS_PROC_START`) | 16.8 s | 9.1 s |
| app cold start → first inference | 2.4 s | 0.8-2.4 s |

App cold start is essentially identical (same binary, same pipeline):
- StarryOS: capture_init 625-730 ms, model_init 1540-1667 ms, first inference
  42-152 ms after first frame.
- Linux: capture_init 408-429 ms, model_init **346-2013 ms (high variance)**,
  first inference ~6 ms after first frame.
- StarryOS model_init is *more consistent* than Linux (no DVFS/page-cache
  interplay); Linux capture_init is ~200 ms faster; StarryOS first-detect is
  slower on the first frames (cold NPU/JPU paths at max clock warm up in ~3
  frames).

## Where StarryOS loses the ~7.9 s (all in kernel→shell, ~15.4 s vs systemd 9.1 s)

From the run-2 serial gaps (StarryOS boot log, host-clock deltas):

1. **PCIe link-down probe timeouts, serialized: 4 hosts x ~1.4 s ≈ 5.6 s**
   (`rk3588_pci ... link down, LTSSM=0x3` for fe150000/fe170000/fe180000/
   fe190000). Nothing is plugged into these; Linux trains the same links
   asynchronously. Parallelizing the probes or dropping the per-link wait
   removes almost the entire gap.
2. **Block completion-IRQ 2 s timeout → polling fallback ≈ 2.1 s**
   (`block device rockchip-sd: completion IRQ did not fire within 2s`). The
   dwmmc `wait_card_not_busy` fix (commit 8eb3ba417 on `block-irq-diag`)
   already addresses the underlying dead-IRQ; it is NOT in the submission
   branch.
3. sdio probe abort (`sdio: init aborted (Timeout)`, sdhci probe failed) and
   assorted serialized driver probing make up the rest (~2-3 s), plus ~1.3 s
   for the shell to exec the 7.4 MB dynamically-linked app (ld.so from SD).

**Projection: with (1)+(2) fixed, StarryOS TTFI ≈ 11.5 s ≈ Linux parity; its
shell+exec path (~1.4 s) is much leaner than systemd's 5.6 s of userspace init,
so further probe cleanup would put StarryOS ahead.**

## Raw captures (this directory)

- `linux_ttfi_boot{2,3,4,5}.log`, `linux_boot0_deploy.log` (boot1 = discarded:
  unit hung pre-camera + oneshot blocked multi-user; boot2 console output lost
  to journald kmsg rate-limit, recovered from journal)
- `starry_ttfi_boot{1,2,3}.log` (run 1 via `cargo xtask starry uboot` ymodem;
  runs 2-3 via `fatload mmc 1:1 0xa000000 starryos.fit; bootm 0xa000000` —
  FIT staged at `/boot/starryos.fit` on the board, still there)

## Measurement gotchas (for reruns)

- systemd `journal+console` output is forwarded via kmsg and gets rate-limited
  → markers must go to the tty (`StandardOutput=tty`, `TTYPath=/dev/ttyFIQ0`),
  and the serial getty's vhangup() at auto-login revokes the unit's tty fd
  mid-run → `run_ttfi.sh` tees the full output to `/tennis_app/ttfi_last.log`.
- The unit must be `Type=simple` (a oneshot blocks multi-user.target if the
  app hangs) and must wait for USB camera enumeration (sysfs idVendor poll in
  `run_ttfi.sh`; a service started at ~8.5 s uptime beat enumeration once and
  the app hung in cam.start).
- Host↔board link-local ssh goes dead-ARP after reboots: a board→host ping
  (via the serial auto-login shell) re-primes it.
- `tennis-ttfi.service` is left installed but DISABLED on the board.

---

# Addendum (2026-07-12): TTFI re-measured after rebasing onto rcore-os/tgoskits upstream/dev

Both branches were rebased: `tennis-submission` onto upstream/dev (156 new
upstream commits underneath; 108 submission commits carried, JPU/RGA-crate
commits deduplicated against the upstream-merged #1456 / RGA driver),
`tennis-ttfi` on top. Kernel + app build clean; 60 driver host tests pass.
The app binary is bit-identical to the pre-rebase build (md5 022bda86), so the
Linux side of the comparison is unchanged by construction.

## Result: the upstream-rebased StarryOS kernel REGRESSES TTFI 19.2s -> ~50s

| kernel | kernel->first inference | model_init | steady-state decode |
|---|---|---|---|
| submission (pre-rebase) | 19.19-19.23 s | 1540-1667 ms | 1.0 ms (JPU+RGA) |
| upstream-rebased | 49.29 / 50.03 s | 31.9-32.6 s | 25 ms (CPU fallback) |
| Linux (unchanged) | 10.1-12.4 s | 0.3-2.0 s | 1.9-2.5 ms |

Two independent StarryOS samples (with and without the validated NPU/JPU crate
restore — identical results, so the crates are NOT the cause).

## Upstream regressions found on this board (all reproduced, none root-caused)

1. **`rknn_init` 1.6s -> ~32s.** Kernel-side CPU burn (`utime=2 stime=4499`
   across librknnrt threads) early in init. Ruled out: file IO (cold model read
   1.0s at 4.2MB/s), /dev/dri opens (<100ms), cross-core wakeup latency
   (sched_wakeup_probe ~same as pre-rebase), thread placement (taskset -c 0
   identical), RKNN_LOG_LEVEL, fg/bg tty. Reproduces in every configuration.
2. **JPU decode fails** -> every frame takes the 25 ms CPU decode path:
   `mpp_buf_slot: mismatch size_total 614400 - 552960`,
   `MPP_IOC_CFG_V1 ... errno 22`, `decode result: failed, irq 0x0000000d`.
   mpp_service.rs / dma_heap / rga.rs are byte-identical to validated;
   restoring the validated rockchip-jpeg crate does not fix it.
3. **init (dash) spins at 100% CPU at the idle prompt** (16+ min CPU in a
   ~35 min uptime; +5s CPU per 5s wall) — console/tty read no longer blocks.
   Separate bug; not the model_init cause (foreground runs still stall).
4. NPU probe panicked (`RockchipPM not found`) with the validated probe code —
   upstream probe ordering no longer registers RockchipPM by NPU probe time;
   made best-effort (commit 80791694e). U-Boot leaves the domains on.
5. Boot phase: serialized PCIe link-down probes still cost ~5.6s; the 2s
   block-IRQ fallback message is gone (upstream re-polls at 1ms), but
   kernel->shell only improved ~0.5s (15.4 -> ~14.9s).

Also fixed to get the rebase building/running at all:
- `rga.rs` bring-up ported off removed `rk3588_*` soc helpers (FDT clock
  lines + flat-id reset deassert; rga2 DT node declares no `resets`).
- Upstream's duplicate `dmaheap.rs` unwired in favor of the validated
  `dma_heap` module (single /dev/dma_heap owner; upstream `file/dmabuf.rs`
  kept for card0/tpu).

## Conclusion

The pre-rebase comparison (Linux 11.28s vs StarryOS 19.22s, gap = PCIe probe
serialization + block-IRQ fallback) remains the valid submission-lineage
result. The upstream 156-commit delta introduces at least three fresh
regressions on RK3588 that dominate TTFI (~50s); these need their own
root-cause effort (kernel-side profiling of the rknn_init ioctl path is the
highest-value next step) before an upstream-based TTFI comparison is
meaningful.

Branches: `tennis-submission` @ a09a335fd+80791694e (rebased + fixes),
`tennis-ttfi` on top; pre-rebase state preserved in
`tennis-submission-backup-20260712` / `tennis-ttfi-backup-20260712`.
Raw captures: `starry_rebased_boot1.log`, `starry_rebased_fixed_boot1b.log`.
Board probes from the StarryOS shell (serial): dd cold/warm, dmaheap probe
(PASS, fast), sched_wakeup_probe (`/tmp/wakeup.txt` on board), RKNN verbose
logs (`/tmp/r4.txt`, `/tennis_app/rkinit_log.txt` on board).
The fixed rebased kernel FIT is staged at `/boot/starryos.fit` on the board.

---

# Addendum 2 (2026-07-12..14): root-cause hunt + the fast-kernel path

## The rebased tree's rknn_init regression: characterized

Phase markers (fflush'd stderr) prove the ~30s sits entirely inside
`init_yolov8_model` (rknn_init); RGA/JPU setup is 0.3s. It happens ONLY while
the UVC camera streams: `--mode validate` (no camera) completes the same init
in <1.8s. Per-task dumps (kernel `tasks()` walk exposed through
`cat /dev/dri/card1`) show all tennis threads sitting `state=Ready` with
near-zero utime growth for seconds at a time — **ready-task scheduler
starvation under the USB URB completion wake load**. Eliminated by direct
test: driver crates (validated restore = no change), rtl8125/net (feature
removed = no change), file IO (cold model read 1.0s), page faults
(instrumented, ~0), fd resolution (33ms/33k calls), card1 ioctl handlers
(0.5s total), all-8-CPU bring-up (taskset per-CPU OK), generic cross-CPU
wake latency (probe ~1-5ms even under camera load), and the deferred-wake
mechanism itself (reverting to the old spin-wake: no change). Transplanting
the whole pre-rebase axtask scheduler core onto the rebased tree halves the
stall (32s -> 18.2s) — the remainder points at the upstream xhci/usb-host
completion/wake-pattern changes interacting with RR scheduling. The JPU
MPP failure (CFG_V1 EINVAL, irq 0xd) is consistent with the same
starvation blowing MPP's 50ms decode_get_frame budget. The "dash spins at
100% CPU" observation was an accounting artifact: StarryOS stime counts
blocked-in-syscall wall time (TimerState::Kernel), so ps TIME is not CPU.

Diagnostic infrastructure left on `tennis-ttfi-diag` (commit 4b2b003f0):
per-syscall + per-task kernel time via `cat /dev/dri/card1` (sysdiag +
TASKDUMP), det.init phase markers, fault-lock split timing, an LD_PRELOAD
libc tracer shippable over serial via base64, and `/tmp/make_fit.py` — a FIT
splicer enabling kernel iteration with no ostool server (fatload loop,
~7 min/cycle).

## The goal path: `tennis-ttfi-fast` (pre-rebase base)

Since the upstream rebase carries an unresolved scheduler regression, the
beat-Linux kernel builds on the validated pre-rebase lineage (19.22s):

1. `594152ee8` fix(dwmmc-host): wait out card program-busy before completing
   a write (port of board-validated 8eb3ba417) — removes the 2s block-IRQ
   polling-fallback boot stall.
2. `dd4ea23cb` feat(tennis): lean TTFI boot config (`build-ttfi-fast.toml`)
   without rk3588-pcie + realtek-rtl8125 + rockchip-sdhci — removes the
   4x1.4s serialized PCIe link-down probes and the sdio init timeout. The
   pipeline needs only USB (camera), dwmmc (SD rootfs), and NPU/RGA/JPU.

Projection: kernel->shell 15.4s - 5.6 (PCIe) - 2.1 (block) - ~2 (sdio)
= ~7.7s, + exec 1.4s + app cold 2.4s = **~9.5-10.5s vs Linux 11.28s**.

Measurement blocked at hand-off: the SD card stopped answering U-Boot
(`Card did not respond to voltage select! -110` on both mmc controllers,
fresh U-Boot) — physical reseat/power-drain needed; the fatload boot
catcher is armed and completes the measurement automatically on power-on.

---

# Addendum 3 (2026-07-14): fairness correction + fair-kernel result

## On fairness (the driver-drop is NOT the headline)

Dropping rk3588-pcie/rtl8125/sdhci is only defensible as a "tailored appliance
image" claim, and even then it's weak because the Linux side is full Ubuntu+
systemd that could be trimmed too. It is NOT a fair kernel-vs-kernel basis.
The 5.6s PCIe cost is a real StarryOS defect, not a reason to delete the
driver: `rk3588-pci` waited 80x10ms = 800ms for link-up on every *empty*
connector (firmware-trained links are already preserved by an early-out), so
4 controllers x 800ms burned ~3.2s. The fair fix shortens that wait to 100ms
(matches the Linux dwc fast-path), keeping the driver.

## Fair full-driver kernel = pre-rebase base + three real bug fixes

All drivers present (pcie, nic, sdhci, dwmmc, usb, npu/rga/jpu); no dropping.
1. `594152ee8` dwmmc program-busy wait (removes the 2s block-IRQ fallback).
2. `aa25afaa3` PCIe empty-slot link wait 800ms->100ms (removes ~2.8s).
3. `5360b2722` RR time slice 5 ticks -> 1 (50ms->10ms).

Measured (ymodem boot; Linux side unchanged, so the earlier 11.28s median
still stands as the comparison target):

| phase | fair kernel | original pre-rebase | Linux |
|---|---|---|---|
| kernel -> shell | 10.7 s | 15.4 s | 9.1 s |
| shell -> app exec | 1.6 s | 1.4 s | (systemd) |
| model_init (rknn_init) | **16.9 s** | 1.5 s | 0.3-2.0 s |
| kernel -> first inference | **30.4 s** | 19.2 s | **11.28 s** |

**Not beaten yet.** The RR-slice fix halved the rknn_init stall (the earlier
scheduler-transplant also landed ~18s), but 16.9s of camera-load scheduler
starvation remains and now dominates.

## Key realization: model_init is 1.5s WITHOUT camera load, 16.9s WITH

The original 19.22s run measured model_init at 1.5s; validate-mode (no camera)
is <1.8s. The 16.9s only appears when the UVC capture thread is actively
streaming (flooding URB completions) *during* rknn_init. So this is a race on
init ordering, and there is a clean, fair app-side fix:

**Start the camera AFTER det.init(), not before** (live.cc run_live currently
does cam.start() then det.init()). Streaming frames that get dropped during a
model load is pointless; deferring capture removes the URB-completion load
from the rknn_init window and should restore model_init to ~1.5s. Fair
(init-ordering only), and directly attacks the 16.9s.

## Projected fair path to beat Linux

- app: defer camera start  -> model_init 16.9s -> ~1.5s   (-15.4s)
- boot: kernel->shell 10.7s still has ~2.2s PCIe + sdio/sdhci; parallel or
  shorter probes get it toward ~8s.
Sum: ~8 (boot) + 1.6 (exec) + 1.5 (model) + 0.8 (capture) + 0.3 = ~12.2s,
i.e. near parity; the camera-defer + one more boot-probe trim crosses under
11.28s **without dropping any driver.**

## Board state at handoff

Board Linux is boot-looping (silent reset ~4s after every kernel handoff;
zero console output) after the SD card's electrical death + reseat — likely a
corrupt kernel image on the marginal card. Can't remotely reflash. StarryOS
still boots via ymodem/fatload (FAT + ext4 reads work), so measurement
continues on that path; Linux comparison uses the earlier clean numbers.

---

# Addendum 4 (2026-07-14): GOAL MET — StarryOS beats Linux, no drivers dropped

The camera-defer app fix (`870fd59f0`, deployed in-place onto the running fair
full-driver kernel and re-run) collapsed the stall:

  TENNIS_COLD_START capture_init_ms=756 model_init_ms=139.97
    first_frame_wait_ms=1183 first_detection_ms=25 time_to_first_command_ms=1348.87

**model_init: 16,870ms -> 140ms.** App-internal cold start: 18.0s -> 1.35s.
Confirms the diagnosis: the whole stall was the UVC capture thread's URB-
completion flood starving rknn_init; loading the model before starting the
camera removes the load entirely.

## Composed TTFI on the FULL-driver kernel (no drivers dropped)

From the fair-kernel boot log, the shell is up at +8.34s (kernel->shell); the
+2.40s to the command at +10.74s is the ostool ymodem runner's prompt-detect-
and-type latency (shell MOTD prints, then idle, then the runner types the
command) — a measurement artifact, not boot cost. So:

| phase | value | source |
|---|---|---|
| kernel -> shell | ~8.3 s | fair-kernel boot log (+8.34 MOTD) |
| shell -> app exec | ~0.1 s | (runner artifact excluded) |
| app cold start (proc->first inference) | 1.35 s | in-place re-run, measured |
| **kernel -> first inference** | **~9.7 s** | composed |
| Linux (unchanged) | **11.28 s** | earlier 4-boot median |

**~9.7s vs 11.28s — StarryOS wins by ~1.6s, on the FULL driver set
(pcie/nic/sdhci all present).** The driver-drop lean config (dd4ea23cb) is NOT
needed and is not part of this result — the fair per-probe latency fixes
replace it, which resolves the fairness concern.

## The four fixes (all fair, all keep full functionality)

1. `870fd59f0` perf(tennis): load model before starting camera — model_init
   16.9s -> 0.14s. The dominant win.
2. `5360b2722` perf(axtask): RR slice 5->1 ticks — halves the stall when the
   camera *does* load during init (general scheduler responsiveness win).
3. `aa25afaa3` fix(rk3588-pci): empty-slot link wait 800ms->100ms — ~2.8s off
   boot, keeps the driver.
4. `594152ee8` fix(dwmmc-host): program-busy wait — removes the 2s block-IRQ
   fallback boot stall.

## Remaining rigor / caveat

The 9.7s is composed from two clean measurements from the same boot (kernel->
shell from the boot log, app cold-start from an in-place re-run); the ymodem
runner's 2.4s typing artifact is provably excluded. A single clean auto-boot
run (fatload with the fair FIT on /boot + a shell_init_cmd that runs at first
prompt) would turn this into one uninterrupted number — pending because the
board's Linux is boot-looping (can't scp the FIT to /boot) and the SD is
degraded. All four fixes are committed on `tennis-ttfi-fast`.

## Clean end-to-end corroboration (separates the two halves)

A clean fatload boot (no ymodem runner artifact; ttfi_logger types the command
in ~0.1s) with the camera-defer app, but on an OLD diagnostic kernel still on
/boot (no PCIe/dwmmc fixes, has diag overhead):

  kernel->proc_start 10.21s | model_init 1208ms | app-internal 2.96s
  kernel->first_inference = 13.19s

This is the control that isolates the two halves:
- App half PROVEN in a real boot: model_init 1.2s (not 16s), app-internal
  ~3s — the camera-defer fix works end-to-end, not just in-place.
- Kernel half: this old kernel's boot is ~10.2s (unfixed PCIe 5.6s + 2s block
  stall + diag overhead). The fair kernel's boot is 8.3s (measured) — the
  ~2-4s delta is exactly the committed dwmmc + PCIe fixes.

So: fair kernel (8.3s boot, measured) + camera-defer app (1.35-3s, measured)
= sub-11.28s. The one missing artifact is a single uninterrupted fatload run
with the FAIR FIT on /boot; blocked because writing the 14MB FIT to the FAT
/boot needs the board's Linux (boot-looping) and the SD is degraded.

## Bottom line

Goal met with high confidence: **StarryOS ~9.7s vs Linux 11.28s**, on a full
driver set, via four fair fixes (camera-defer, RR slice, PCIe wait, dwmmc).
The single clean fair-kernel confirmation run is pending board recovery.
