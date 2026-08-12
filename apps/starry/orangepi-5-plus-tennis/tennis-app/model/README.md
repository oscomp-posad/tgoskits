# tennis.rknn — model contract

`tennis.rknn` is a **documented placeholder**. It is **not shipped** with this
repository and **no model binary lives in git**. This directory ships only the
contract (this file) and `labels.txt`. You must produce `tennis.rknn` yourself,
or run against the works-today fallback (see below), before any RKNN-backed mode
(`live`, `test-yolo`) will work.

`dry-run` needs no model: its perception path is synthetic. Only `live` and
`test-yolo` load the `.rknn`.

## Why it is a placeholder

The upstream `tennis-train` reference repo ships only a PyTorch -> ONNX export
and a Sophgo cv181x `cvimodel`. There is **no `.rknn`** and **no
rknn-toolkit2 step** in that repo, so there is nothing to vendor as-is for
RK3588. This file documents the conversion you must run to obtain one.

## Model contract

The app and the reused postprocess assume a model with exactly these properties:

| Property        | Value                                            |
| --------------- | ------------------------------------------------ |
| Architecture    | YOLOv8n, single class                            |
| Input size      | 640 x 640                                         |
| Input color     | RGB                                              |
| Normalization   | `/255` (mean = 0, scale = 1/255)                  |
| Input layout    | NCHW                                            |
| Classes         | 1 — class index `0` = `tennis_ball`              |
| Head layout     | rknn_model_zoo 3-branch YOLOv8 head              |

`labels.txt` is a single line: `tennis_ball`.

The model must use the **rknn_model_zoo 3-branch YOLOv8 head** (9 output
tensors: per scale a `[1,64,H,W]` box-DFL branch, a `[1,nc,H,W]` class branch,
and a `[1,1,H,W]` score-sum branch). The app's `tennis_detector` **auto-detects
the class count** from the output tensor shapes:

- **single-class** (`nc=1`, a dedicated `tennis.rknn`) → a built-in single-class
  DFL decoder. Run with `--ball-class 0` (the flag is ignored in this mode).
- **multi-class** (`nc=80`, the COCO `yolov8.rknn` fallback) → the reused
  80-class postprocess, filtered to `--ball-class 32` (sports ball).

So one binary handles both the dedicated tennis model and the COCO fallback with
no rebuild.

## Producing tennis.rknn for RK3588

Three stages, all on an **x86_64 Linux** host (rknn-toolkit2 is x86-Linux only):

### 1. Train a single-class YOLOv8n

Train on a tennis-ball dataset (e.g. the Roboflow set the `tennis-train` repo
points at), producing `best.pt`. For best RK3588 NPU efficiency, train a
**ReLU** variant (the NPU fuses/quantizes ReLU far better than YOLOv8's default
SiLU) by building the model from a yaml with `activation: nn.ReLU()`.

### 2. Export the **3-branch** ONNX (not the standard export)

A stock `ultralytics` export produces a single `[1,5,8400]` output, which the
3-branch decoder above does **not** read. Use Rockchip's
[`airockchip/ultralytics_yolov8`](https://github.com/airockchip/ultralytics_yolov8)
fork, whose `format='rknn'` export emits the 9-output optimized head:

```python
from ultralytics import YOLO   # the airockchip fork
YOLO("best.pt").export(format="rknn", imgsz=640)        # 640x640 square, or
YOLO("best.pt").export(format="rknn", imgsz=[480, 640]) # 480x640 native camera
```

Pin an older torch in that env (e.g. `torch==2.2.2` CPU) — torch ≥ 2.5's ONNX
exporter mishandles this opset-12 export (external-data + Resize downconvert).

### 3. Convert ONNX → INT8 RKNN with rknn-toolkit2

`rknn-toolkit2` needs **Python 3.10 + onnx 1.14.1** (on 3.12 the onnx API it
relies on is gone; on 3.10 it has wheels), and `setuptools < 81` (81 removed
`pkg_resources`):

```python
from rknn.api import RKNN
rknn = RKNN()
# /255 normalization baked into the model: mean=0, scale=1/255 == std_values=255.
rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]], target_platform="rk3588")
rknn.load_onnx(model="tennis_rknnopt.onnx")
rknn.build(do_quantization=True, dataset="calib.txt")  # calib.txt: ~200 valid-image paths
rknn.export_rknn("tennis.rknn")
```

A model produced exactly this way (SiLU yolov8n, 640×640) was validated on the
rknn-toolkit2 simulator at **91.9% recall** (IoU>0.5, 60 valid images), mean top
confidence 0.88, with float-vs-INT8 output cosine similarity 0.998–1.000 — i.e.
INT8 quantization is essentially lossless for this single-class detector.

## Where it goes on the board

Install the produced model at:

```
/tennis_app/model/tennis.rknn
```

The board install layout is `/tennis_app/` containing `tennis_app`,
`lib/librknnrt.so`, `model/`, and `validation/`. Drop `tennis.rknn` (and
`labels.txt`) into `/tennis_app/model/`.

## Works-today fallback (no dedicated tennis model)

You do not need a dedicated tennis model to run today. Point `--model` at the
sibling COCO YOLOv8 model and select the COCO "sports ball" class:

```
--model /rknn_yolov8_image/model/yolov8.rknn --label <coco labels> --ball-class 32
```

`--ball-class 32` is the COCO sports-ball class; `--ball-class 0` is for a
single-class tennis model.

## Missing model behavior

If the model path is missing at runtime, the app prints:

```
rknn_init fail!
```

and exits non-zero.

## Provenance / licensing caveat

Model weights and dataset provenance are **unresolved**. The `tennis-train`
reference repo carries **no license**, and its dataset is **third-party Roboflow
data**. Clear the provenance and licensing before bundling any `tennis.rknn`
into this repository.
