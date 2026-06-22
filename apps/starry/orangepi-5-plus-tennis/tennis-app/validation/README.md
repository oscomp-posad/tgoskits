# Fixed-image validation (OPTIONAL — documented placeholder)

This directory is **optional** and is currently a **documented placeholder**. The
tennis app does **not** yet implement an on-device fixed-image validation harness,
and no `tennis.rknn` model is shipped (the model is itself a documented
placeholder). The files here describe the *intended* format so the directory is
meaningful; they intentionally contain **no real image entries and no golden
detections** yet. Adding the validation harness is a follow-up.

## What this would become

Once two things exist together:

1. an on-device **fixed-image validation harness** in the tennis app, and
2. a **pinned `tennis.rknn`** model (single-class YOLOv8n tennis-ball detector),

this directory would hold the deterministic, model-pinned regression set:

- **`images.txt`** — one image path per line, **relative to `/tennis_app`** (the
  install root on the board). For example, an image stored at
  `/tennis_app/validation/foo.jpg` would be listed as `validation/foo.jpg`.
- **`expected.txt`** — the **golden detections** for those images: the bounding
  boxes / scores the pinned model is expected to produce, so a run can be
  compared against a committed reference and flagged on drift.

Both files are line-oriented text and use `#`-prefixed comment lines for headers
and notes.

## Model to follow

The sibling app **`apps/starry/orangepi-5-plus-uvc-rknn`** already implements
fixed-image validation against its committed RKNN YOLOv8 model. Follow it as the
reference when this harness is built:

- It ships `images.txt` (one board-relative image path per line) and
  `expected.txt` (a versioned, line-oriented golden-detection file with a header
  line plus per-image `image ...` / `det ...` records) under its own
  `validation/` directory, alongside the committed validation images.
- Its benchmark binary consumes those files (`--validate-list images.txt
  --expected expected.txt`) and prints a single PASS marker line on success.

The tennis app would mirror that shape, but with image paths relative to
`/tennis_app` (the sibling installs under `/rknn_yolov8_image` instead) and with
golden detections produced by the pinned single-class tennis model rather than
the COCO model.

## Primary validation today

Until the fixed-image harness and a pinned model exist, the **live / dry-run
benchmark is the primary validation**. The mandatory deliverable is the
`TENNIS_BENCH_RESULT` line:

- **Host (no board, no camera, no model):**

  ```
  cmake -S tennis-app -B tennis-app/build-host -DTENNIS_HOST_DRYRUN=ON
  cmake --build tennis-app/build-host
  ./tennis-app/build-host/tennis_app --mode dry-run --duration-sec 5
  ```

  `dry-run` drives the full state machine from a deterministic synthetic scene
  and emits `TENNIS_BENCH_BEGIN` / `TENNIS_BENCH_RESULT` / `TENNIS_BENCH_DONE`.
  In `dry-run` the perception path is synthetic, so `frame_to_detection_ms` is
  ~0 and the latencies reflect control-loop plumbing only.

- **Board live** (`--mode live`, needs a UVC camera + a model): the same
  `TENNIS_BENCH_RESULT` line, but the latencies reflect real MJPEG decode + NPU
  inference.

The competition metric is `frame_to_command_ms` (capture → motor/arm command
latency); `frame_to_detection_ms` is the capture → detection latency.
