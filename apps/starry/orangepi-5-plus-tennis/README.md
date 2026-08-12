# orangepi-5-plus-tennis

A StarryOS board demo and benchmark app that ports the RK3588 tennis-ball pickup robot workflow to an OrangePi-5-Plus (RK3588) + UVC camera, using **virtual** motor and arm backends so it runs today with no physical car.

## What this demonstrates

This app drives the full tennis pickup workflow — chase a tennis ball, grab it, find a red bucket, approach it, and deposit — and frames it as an **end-to-end latency benchmark**. The competition goal is minimizing the time from a camera frame to the motor/arm command that acts on it (frame-to-command latency), and the mandatory deliverable is the `TENNIS_BENCH_RESULT` line. Perception (YOLOv8 ball detection + HSV bucket detection) and control (a differential-drive state machine) run on the board's NPU and CPU; the actuators are virtual command traces, so the measured latency is the pure perception+control compute path on StarryOS.

## Why virtual actuators

The virtual motor/arm backends give a clean, **non-blocking** seam between the control loop and the hardware:

- It **runs with no physical car** — only the OrangePi-5-Plus board and a UVC camera (for live modes) are required.
- Because the virtual actuator backend never blocks, **frame-to-command latency stays pure compute** — there is no UART round-trip or servo settle time inflating the benchmark.
- The **real UART backends are future, hardware-gated work**: a differential-motor backend (ESP32-C3) and a gripper-arm backend (ZP10D bus servo) are designed for but disabled until the car arrives. They are not the default.

## Architecture

The pipeline is built for low frame-to-command latency:

- **Capture thread (drop-old):** `libuvc` runs its own capture thread and latches only the **newest** frame into a sequenced latest-frame slot (frame id + capture timestamp). Old frames are dropped rather than queued.
- **Latest-frame slot:** the perception/control loop reads the most recent frame from this slot, so it never works on stale backlog.
- **Perception + control loop:** reads the latest frame, runs YOLOv8 **or** HSV per the current state (never both — no redundant decode), runs the state machine, and calls the **non-blocking** virtual actuator backend. Because the actuator never blocks, frame-to-command latency = pure compute.
- **Multi-core NPU:** inference runs across all three RK3588 NPU cores via `rknn_set_core_mask` (`RKNN_NPU_CORE_0_1_2`). The stock image/stream binaries do not set this.
- **Single-class postprocess:** the YOLOv8 head is decoded for a single tennis-ball class. No per-frame heap allocation in the steady state.
- **HSV bucket detection:** the bucket is found with an HSV red-bucket detector rather than a second NPU pass.

**PR1 fuses perception + control on one thread.** This is safe precisely because the virtual actuator is non-blocking — there is nothing to wait on. A separate control thread plus multiple `rknn_dup_context` NPU workers is a deferred optimization that only matters once a blocking real-UART backend lands.

## Reuse (no duplication)

The board build **reuses** the vendored, Apache-2.0 (Rockchip) RKNN/UVC code from the sibling app `apps/starry/orangepi-5-plus-uvc-rknn/rknn-yolov8-image` instead of copying it. Via the CMake cache variable `TENNIS_RKNN_SHARED_DIR` (default `../../orangepi-5-plus-uvc-rknn/rknn-yolov8-image`) it pulls in:

- the sibling's `3rdparty/` libraries (`librknnrt.so`, `librga`, `libturbojpeg`, `stb_image`, rknn headers),
- its `utils/` (`image_utils`, `image_drawing`, `file_utils`),
- and its `cpp/` modules (`uvc_capture`, `rknpu2/yolov8`, `postprocess`).

The multi-MB libraries and the model are **not** copied. The tennis-specific code — state machine, `tennis_detector` wrapper, HSV `bucket_detector`, virtual backends, metrics, `main`, and the live pipeline — is new and lives under `tennis-app/cpp`.

## Build

### Host self-test (dry-run, no board/camera/model)

```bash
cmake -S tennis-app -B tennis-app/build-host -DTENNIS_HOST_DRYRUN=ON
cmake --build tennis-app/build-host
./tennis-app/build-host/tennis_app --mode dry-run --duration-sec 5
```

`-DTENNIS_HOST_DRYRUN=ON` produces a self-contained native build with no RKNN/UVC/RGA dependency; `--mode dry-run` is the only mode in that build. It drives the full state machine from a deterministic synthetic scene and emits `TENNIS_BENCH_RESULT`, so the benchmark is reproducible on any host (and in CI).

### Board cross-build

Install an aarch64 Linux **glibc** cross toolchain (the prebuilt Rockchip libs are glibc — do **not** use a musl toolchain):

```bash
# Linux / tgoskits container
sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# macOS (Homebrew)
brew install messense/macos-cross-toolchains/aarch64-unknown-linux-gnu
```

Then:

```bash
./build-image-runner.sh
```

`build-image-runner.sh` auto-detects the cross prefix (`aarch64-linux-gnu-`, else `aarch64-unknown-linux-gnu-`; override with `CROSS_COMPILE=`). It builds with `CMAKE_SYSTEM_NAME=Linux`, `TARGET_SOC=rk3588`, Release. The output lands in:

```
tennis-app/install/rk3588_linux_aarch64/tennis_app/
```

containing the `tennis_app` binary + `lib/librknnrt.so` + `model/` + `validation/`. `libstdc++`/`libgcc` are statically linked so the binary does not depend on the board's C++ runtime version; glibc stays dynamic (the board's glibc must be ≥ the toolchain's — use the container toolchain if you hit a `GLIBC_…` runtime error). The binary's only bundled runtime dependency is `librknnrt.so` (via `RPATH=$ORIGIN/lib`); `libuvc` is `dlopen`'d at runtime for live modes only.

## Run on the board

Run through xtask:

```bash
# Default: bounded dry-run smoke (no camera/model needed)
cargo xtask starry app board -t orangepi-5-plus-tennis

# Live 60s perception benchmark (camera + model required)
cargo xtask starry app board -t orangepi-5-plus-tennis --board-config configs/board-orangepi-5-plus-bench.toml

# Live indefinitely
cargo xtask starry app board -t orangepi-5-plus-tennis --board-config configs/board-orangepi-5-plus-long-run.toml
```

- The **default** board config (`board-orangepi-5-plus.toml`) runs a bounded **dry-run smoke** — no camera or model needed. It proves the binary runs and emits the benchmark output on real StarryOS.
- `configs/board-orangepi-5-plus-bench.toml` runs the **60s live** perception benchmark (needs camera + model).
- `configs/board-orangepi-5-plus-long-run.toml` runs live indefinitely.

**The xtask deploys the StarryOS kernel only.** The `tennis_app` binary + libs + model must already be installed on the board rootfs (see below).

## On-board install

The board runs above are gated on a manual install:

1. Build with `./build-image-runner.sh` on a Linux host.
2. Copy `install/rk3588_linux_aarch64/tennis_app/` to the board at `/tennis_app` (rsync over SSH while holding a board lease), then `chown root:root` and `sync`.
3. `/tennis_app` must contain: `tennis_app`, `lib/librknnrt.so`, `model/`, `validation/`.

For **live** modes, `libuvc` must also be present on the board (e.g. `/usr/lib/aarch64-linux-gnu/libuvc.so`) and a UVC camera connected. **dry-run** needs only the binary + `lib/librknnrt.so`.

## Required runtime assets

- **`model/tennis.rknn` — documented placeholder, not shipped.** The `tennis-train` reference repo ships only a PyTorch→ONNX export and a Sophgo cv181x cvimodel — there is no `.rknn`, no rknn-toolkit2 step, and no LICENSE (the dataset is third-party Roboflow data). The model is a single-class YOLOv8n at 640x640, RGB, `/255` normalization (mean=0, scale=1/255), input NCHW, single class `tennis_ball`.

  To produce `tennis.rknn` for RK3588: take the FP32 ONNX, then use rknn-toolkit2:
  ```python
  rknn.config(mean_values=[[0,0,0]], std_values=[[255,255,255]], target_platform='rk3588')
  rknn.load_onnx(...)
  rknn.build(do_quantization=True, dataset=<list of valid images>)
  rknn.export_rknn('tennis.rknn')
  ```
  The reused postprocess decodes the `rknn_model_zoo` 3-branch YOLOv8 head, so export the model in that layout (single class).

- **COCO fallback (works today, no dedicated tennis model):** point `--model` at the sibling COCO model (e.g. `/rknn_yolov8_image/model/yolov8.rknn`), pass its COCO labels via `--label`, and set `--ball-class 32` (COCO sports ball).

- **`model/labels.txt`** — one line: `tennis_ball` (for the single-class tennis model). For the COCO fallback, use the COCO labels file instead.

> If the model path is missing at runtime the app prints `rknn_init fail!` and exits non-zero.

## Modes

```bash
tennis_app --mode live --model model/tennis.rknn --label model/labels.txt --device 0 --width 640 --height 480 --fps 30 --duration-sec 60 --virtual-actuators
tennis_app --mode test-uvc --device 0
tennis_app --mode test-yolo --model model/tennis.rknn --device 0
tennis_app --mode test-bucket --device 0
tennis_app --mode dry-run --duration-sec 10 --virtual-actuators
```

`dry-run` needs no camera/model/board and emits `TENNIS_BENCH_RESULT`. The test modes additionally print `TENNIS_TEST_UVC ...` + `TENNIS_TEST_UVC_DONE`, `TENNIS_TEST_YOLO ...` + `TENNIS_TEST_YOLO_DONE`, and `TENNIS_TEST_BUCKET ...` + `TENNIS_TEST_BUCKET_DONE`.

## CLI flags

| Flag | Default | Meaning |
|------|---------|---------|
| `--mode <live\|test-uvc\|test-yolo\|test-bucket\|dry-run>` | — | Run mode |
| `--model <path>` | `model/tennis.rknn` | RKNN model path |
| `--label <path>` | `model/labels.txt` | Labels file |
| `--device <n>` | `0` | UVC device index |
| `--width <n>` / `--height <n>` | `640` / `480` | Capture resolution |
| `--fps <n>` | `30` | Capture frame rate |
| `--duration-sec <f>` | `60` | Run duration in seconds |
| `--virtual-actuators` | on | The only actuator path (flag) |
| `--ball-class <n>` | `0` | `0` for a single-class tennis model; `32` = COCO sports ball |
| `--min-confidence <0-100>` | `50` | Detection confidence threshold |
| `--log-every <n>` | `1` | Emit per-frame lines every Nth frame |
| `--staleness-ms <n>` | `0` | `0` = never drop on age |
| `--core-mask <m>` | `all` | One of `all\|auto\|0\|1\|2\|0_1\|0_1_2` |

## Example output

A 5s host dry-run. In dry-run the perception path is synthetic, so `frame_to_detection` is ~0 and the latencies reflect control-loop plumbing only — **live** numbers reflect real MJPEG decode + NPU inference.

```
TENNIS_BENCH_BEGIN mode=dry-run fps=30 duration_sec=5.000 virtual_actuators=1
TENNIS_STATE frame=0 state=CHASE_BALL detections=1 bucket_visible=0 frame_age_ms=0.000
TENNIS_CMD frame=0 state=CHASE_BALL motor_left=41 motor_right=31 arm_action=none capture_ts_ns=24151827553000 decision_ts_ns=24151827566000 frame_to_command_ms=0.013
TENNIS_BENCH_RESULT duration_sec=5.003 captured=150 processed=150 detections=98 bucket_detections=42 virtual_motor_commands=76 virtual_arm_commands=5 frame_to_detection_ms_avg=0.000 frame_to_detection_ms_p50=0.000 frame_to_detection_ms_p95=0.001 frame_to_command_ms_avg=0.003 frame_to_command_ms_p50=0.002 frame_to_command_ms_p95=0.006 decode_errors=0 inference_errors=0 memory_rss_kb=1360
TENNIS_BENCH_DONE
```

The full set of verbatim output-line formats:

```
TENNIS_BENCH_BEGIN mode=<...> ...
TENNIS_STATE frame=<id> state=<CHASE_BALL|GRAB|FIND_BUCKET|APPROACH_BUCKET|DEPOSIT> detections=<n> bucket_visible=<0|1> frame_age_ms=<...>
TENNIS_CMD frame=<id> state=<...> motor_left=<l> motor_right=<r> arm_action=<none|grab|release|ready> capture_ts_ns=<...> decision_ts_ns=<...> frame_to_command_ms=<...>
TENNIS_MOTOR drive left=<l> right=<r>      (also: TENNIS_MOTOR brake / TENNIS_MOTOR standby) — emitted only when the command changes
TENNIS_ARM grab|release|ready              — emitted by the virtual arm backend
TENNIS_BENCH_RESULT ...
TENNIS_BENCH_DONE
```

## Benchmark fields

Fields on the `TENNIS_BENCH_RESULT` line:

- `duration_sec` — measured run duration in seconds.
- `captured` — frames latched by the capture thread.
- `processed` — frames the perception/control loop actually ran on.
- `detections` — frames in which a ball was detected.
- `bucket_detections` — frames in which the red bucket was detected.
- `virtual_motor_commands` — distinct motor commands emitted (command changed).
- `virtual_arm_commands` — arm commands emitted (grab/release/ready).
- `frame_to_detection_ms_avg` / `_p50` / `_p95` — capture→detection latency (avg, median, 95th percentile).
- `frame_to_command_ms_avg` / `_p50` / `_p95` — capture→command latency, the competition metric (avg, median, 95th percentile).
- `decode_errors` — MJPEG decode failures.
- `inference_errors` — NPU inference failures.
- `memory_rss_kb` — process resident set size in KB.

`frame_to_detection` is the capture→detection latency; `frame_to_command` is the capture→command latency (the competition metric).

## Works now (board + camera) vs requires the physical car later

**Works now (board + camera, no car):**

- UVC capture
- MJPEG decode
- RKNN YOLOv8 ball detection
- Full state machine (`CHASE_BALL`, `GRAB`, `FIND_BUCKET`, `APPROACH_BUCKET`, `DEPOSIT`)
- HSV red-bucket detection
- Virtual differential motor + gripper-arm commands
- The `TENNIS_BENCH_RESULT` benchmark

**Requires the physical car later (designed-for but disabled, hardware-gated, not the default):**

- Real UART differential-motor backend (ESP32-C3, `0xAA 0x55` framed protocol @115200)
- Real UART gripper-arm backend (ZP10D bus servo, ASCII `#IDPpulseTtime!` @115200)

## Attribution

The workflow is ported from the reference repos [aka-rk3588](https://github.com/pengzechen/aka-rk3588) and [tennis-train](https://github.com/pengzechen/tennis-train) by **pengzechen**. Those repos carry **no license**, so the state machine, steering math, HSV bucket detector, and actuator backends here are **clean reimplementations** from the documented behaviour, not copied code.

The reused RKNN/UVC/image-utility code is Apache-2.0 (Rockchip) and is shared from the sibling `orangepi-5-plus-uvc-rknn` app (no duplication).

**Model/dataset provenance is unresolved** (no license + third-party Roboflow data). Clear it before bundling any `tennis.rknn` into the repo.

## Next steps

1. Add a real UART motor backend once the car arrives.
2. Add a real UART arm backend once the car arrives.
3. Switch preprocessing to RGA once `/dev/rga` works.
4. Split a dedicated control thread + multiple `rknn_dup_context` NPU workers for true 3-core parallelism.
5. Add a Linux-vs-Starry benchmark comparison.
