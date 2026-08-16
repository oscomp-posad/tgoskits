# STARRY//SIGNAL demo: dashboard + live tennis loop on HDMI

End-to-end runbook for the on-glass demo: StarryOS cold-boots its native
display driver (VOP2 + HDPTX PHY + DW-HDMI-QP, no U-Boot display reuse), the
Qt dashboard renders to `/dev/fb0`, and the tennis pipeline (UVC camera → RKNN
NPU → state machine) streams its camera viewport and telemetry into the
dashboard — all on one kernel. Board-validated 2026-08-17 on an
OrangePi-5-Plus with a 1080p-capable HDMI monitor.

## Hardware

- OrangePi-5-Plus (RK3588), serial console, board Linux on eMMC for deploys.
- HDMI monitor on **HDMI0**. The board has three HDMI-size jacks: the two
  nearest the Ethernet ports are outputs, the one next to the USB 2.0 ports is
  an HDMI **input** (never displays). HDMI0 self-identifies: after boot it
  shows color bars — if your monitor shows nothing, move to the other output
  jack.
- UVC camera in a **blue USB 3.0** port, plugged in **before boot** (no
  hotplug enumeration).

## 1. Kernel

```sh
cargo xtask starry build -c os/StarryOS/configs/board/orangepi-5-plus-vop2coldinit.toml
```

Boot via the standard board flow (`cargo xtask starry uboot -c … --uboot-config
os/StarryOS/configs/board/orangepi-5-plus-uboot.toml`, or a FIT + `fatload`).
Expected on a good boot: SMPTE color bars on the monitor, and in the log
`coldinit: HDPTX PHY locked @ 148.5MHz` … `coldinit: EDID block0 read OK`.

## 2. Userspace (build once, on the host)

```sh
apps/starry/orangepi-5-plus-tennis/dashboard/board-prebuild.sh   # Docker → dashboard-board.tar.gz
apps/starry/orangepi-5-plus-tennis/tennis-app/build-native.sh    # tennis_app binary
```

## 3. Deploy (from the board's Linux, shared ext4 rootfs)

- Untar the dashboard bundle to e.g. `/opt/hud` (contains `dashboard`, `lib/`,
  `plugins/`, `fonts/`).
- Deploy the tennis app per `tennis-app/README.md` (binary + `lib/` + `model/`
  at e.g. `/tennis_app`).
- Copy `run-demo.sh` (this directory) to the rootfs and `chmod +x`; `sync`
  before rebooting into StarryOS.

## 4. Run (StarryOS shell)

```sh
APP_DIR=/tennis_app HUD_DIR=/opt/hud ./run-demo.sh &
```

The dashboard comes up full-screen with the live camera viewport (`无信号`
means no `--publish-camera` feed yet) and telemetry; wave a tennis ball at the
camera and the detection state reacts. Frame-to-command stays ~13–19 ms with
the viewport at 10 fps publish / 15 fps render.

## Caveats (board-proven)

- **Use piped telemetry (as `run-demo.sh` does), not `--telemetry` file
  tailing.** Concurrently tailing a tmpfs file the tennis app is writing can
  wedge the writer in-kernel (unkillable; reboot to recover) — a StarryOS
  filesystem-concurrency bug, tracked as a follow-up. The pipe avoids it.
- **Known bug — stochastic UVC capture stall.** Live camera capture can go
  quiet after minutes to tens of minutes (observed 23 s–15 min; the display
  and dashboard keep running, the viewport just stops updating). The stall
  state lives in the USB stack and survives process restarts — recovery is a
  reboot (~2 min) and rerunning `run-demo.sh`. Tracked as a follow-up against
  the USB host stack (completions are polled, no IRQ). For a staged demo,
  reboot shortly before showing; runs usually last well past 10 minutes.
- The display config does not include the RGA driver; the tennis app falls
  back to CPU resize automatically (fine for the demo; add the RGA feature for
  the zero-copy path).
- Higher `--publish-fps` costs ~8 ms of pipeline time per published frame;
  keep it ≤ 10 for demos, or drop `--publish-camera` for benchmark runs.
