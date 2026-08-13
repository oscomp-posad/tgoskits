# tennis.rknn — deployed model and model contract

`tennis.rknn` is the board-validated default model used by
`configs/orangepi5plus-live.conf`. It is shipped in this repository together
with `labels.txt`, so RKNN-backed modes (`live`, `test-yolo`, and `validate`)
can use the default configuration without separately installing a model.

The current model was fine-tuned from the ReLU classification-head-32 +
`reg_max=8` candidate with 50 reviewed empty-label red-bucket hard negatives.
Its fixed identity is:

```text
file=tennis.rknn
size=3735708
sha256=9d632490152961d66e79b0c80f9cdba19f92e90f29ca140670654646bc33691a
input=480x640 RGB
outputs=6 (box and class tensors at strides 8, 16, and 32)
box channels=32 (reg_max=8)
class head hidden channels=32
quantization=INT8, RKNN Toolkit 2.3.2, target rk3588
```

The two `tennis_relu_*.rknn` files are retained as earlier comparison models;
they are not selected by the default configuration.

`dry-run` needs no model: its perception path is synthetic. The `live`,
`test-yolo`, and `validate` modes load the `.rknn`.

## Model contract

The app and the reused postprocess assume a model with exactly these properties:

| Property        | Value                                            |
| --------------- | ------------------------------------------------ |
| Architecture    | YOLOv8n ReLU, single class                       |
| Input size      | 480 x 640                                        |
| Input color     | RGB                                              |
| Normalization   | `/255` (mean = 0, scale = 1/255)                  |
| Runtime input   | NHWC INT8                                       |
| Classes         | 1 — class index `0` = `tennis_ball`              |
| Head layout     | 3 branches, box + class per branch (6 outputs)   |
| DFL             | 32 box channels, `reg_max=8`                     |

`labels.txt` is a single line: `tennis_ball`.

The dedicated model uses three YOLOv8 scales. Each scale exposes a box-DFL
tensor and a single-class score tensor; the redundant score-sum tensors are
omitted. The detector accepts 32- or 64-channel box tensors and derives
`reg_max` from `box_channels / 4`.

The app's `tennis_detector` also auto-detects the class count from output
tensor shapes:

- **single-class** (`nc=1`, a dedicated `tennis.rknn`) → a built-in single-class
  DFL decoder. Run with `--ball-class 0` (the flag is ignored in this mode).
- **multi-class** (`nc=80`, the COCO `yolov8.rknn` fallback) → the reused
  80-class postprocess, filtered to `--ball-class 32` (sports ball).

So one binary handles both the dedicated tennis model and the COCO fallback with
no rebuild.

## Reproducing tennis.rknn for RK3588

Three stages, all on an **x86_64 Linux** host (rknn-toolkit2 is x86-Linux only):

### 1. Train a single-class YOLOv8n

Train on a tennis-ball dataset (e.g. the Roboflow set the `tennis-train` repo
points at), producing `best.pt`. For best RK3588 NPU efficiency, train a
**ReLU** variant (the NPU fuses/quantizes ReLU far better than YOLOv8's default
SiLU) by building the model from a yaml with `activation: nn.ReLU()`.

### 2. Export the six raw head tensors (not the standard export)

A stock `ultralytics` export produces a single `[1,5,8400]` output, which the
3-branch decoder above does **not** read. Export the three box tensors and three
class-score tensors directly from the detection head, using a fixed
`1x3x480x640` input and ONNX opset 17. For the current model the output shapes
must be:

```text
box_p3   [1,32,60,80]    score_p3 [1,1,60,80]
box_p4   [1,32,30,40]    score_p4 [1,1,30,40]
box_p5   [1,32,15,20]    score_p5 [1,1,15,20]
```

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
rknn.build(do_quantization=True, dataset="calib.txt")  # pinned 64-image calibration list
rknn.export_rknn("tennis.rknn")
```

The current RKNN was evaluated on the original 285-image, 382-box test split:
Precision `0.94536`, Recall `0.90576`, mAP50 `0.91078`, and mAP50-95 `0.70912`.
On the 15-image red-bucket hard-negative holdout it has zero false-positive
images at confidence thresholds `0.70`, `0.72`, and `0.75`.

## Where it goes on the board

Install the produced model at:

```
/tennis_app/model/tennis.rknn
```

The board install layout is `/tennis_app/` containing `tennis_app`,
`lib/librknnrt.so`, `model/`, and `validation/`. Drop `tennis.rknn` (and
`labels.txt`) into `/tennis_app/model/`.

## COCO fallback

For diagnostics, the application can instead use the sibling COCO YOLOv8 model
and select the COCO "sports ball" class:

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

The training dataset originated from third-party Roboflow data and the local
training repository does not provide a standalone dataset license. Keep this
provenance constraint in mind when redistributing the model outside this
project.
