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

The reused postprocess decodes the **rknn_model_zoo 3-branch YOLOv8 head**, so
the exported model must be in that layout (single class). With a single-class
tennis model, run the app with `--ball-class 0`.

## Producing tennis.rknn for RK3588

Start from the FP32 ONNX export (the `tennis-train` repo provides the
PyTorch -> ONNX step) and convert it with `rknn-toolkit2`:

```python
from rknn.api import RKNN

rknn = RKNN()

# Normalization is baked into the model: /255 means mean=0, scale=1/255,
# expressed to rknn-toolkit2 as std_values = 255.
rknn.config(
    mean_values=[[0, 0, 0]],
    std_values=[[255, 255, 255]],
    target_platform="rk3588",
)

rknn.load_onnx(model="tennis.onnx")

# Quantize against a list of representative validation images.
rknn.build(do_quantization=True, dataset="dataset.txt")  # dataset.txt = list of valid images

rknn.export_rknn("tennis.rknn")
```

Steps, in order:

1. `rknn.config(mean_values=[[0,0,0]], std_values=[[255,255,255]], target_platform="rk3588")`
2. `rknn.load_onnx(...)` on the FP32 ONNX.
3. `rknn.build(do_quantization=True, dataset=<list of valid images>)`.
4. `rknn.export_rknn("tennis.rknn")`.

Export the model in the rknn_model_zoo 3-branch YOLOv8 head layout (single
class) so the reused postprocess can decode it.

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
