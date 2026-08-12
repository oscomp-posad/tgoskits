# 第三方组件与归属

本工作在若干开源项目与预编译库的基础上展开。下表列出所用的第三方组件、用途、来源与许可。各项均按其上游许可使用，本队的增量贡献见 `docs/基础版本与增量贡献.md`。

## 一、基础项目

| 组件 | 用途 | 来源 | 许可 |
|---|---|---|---|
| tgoskits（集成 Starry 与 ArceOS）| 操作系统内核基础 | rcore-os/tgoskits | Apache-2.0 |
| aka-rk3588、tennis-train | 机器人工作流与训练流程的参考 | pengzechen | 无许可证，本工作据其公开行为重新实现，未复制代码 |
| rknn-toolkit2 | 将 YOLOv8 转换为 RKNN 格式 | airockchip/rknn-toolkit2 | 工具链 Apache-2.0 |

## 二、随应用内置的运行库

机器人应用在 `apps/starry/orangepi-5-plus-tennis/tennis-app/3rdparty/` 下内置了以下运行库，使其无需在线拉取即可在目标板上构建与运行。各库均原样使用、仅作构建衔接，并保留其上游许可；该目录的逐库说明见同目录 `THIRD_PARTY.md`。

| 库 | 用途 | 来源 | 许可 |
|---|---|---|---|
| librga | RGA 二维缩放与色彩转换的用户库 | airockchip/librga | Apache-2.0 |
| Rockchip MPP（`librockchip_mpp`）| 经 `/dev/mpp_service` 的 MJPEG 硬件解码 | rockchip-linux/mpp | Apache-2.0 |
| RKNPU2 运行时（`librknnrt`）| NPU 推理运行时与 C 头文件 | airockchip/rknn-toolkit2 的 rknpu2 组件 | Rockchip 闭源运行时，按其再分发条款原样附带 |
| libjpeg-turbo | CPU 端 JPEG 回退解码 | libjpeg-turbo/libjpeg-turbo | BSD 类 / IJG / zlib |
| stb_image | 主机自测的轻量图像加载 | nothings/stb | 公有领域 / MIT |

RKNPU2 运行时（`librknnrt.so`）是 Rockchip 的闭源二进制，作为应用链接的预编译共享对象原样再分发，非由源码构建。

## 三、模型与数据集

| 项 | 说明 |
|---|---|
| 检测模型 | 单类 YOLOv8n 网球检测模型，由本队训练并经 rknn-toolkit2 导出为 RKNN（480×640 与 640×640 两种输入），权重随应用提供 |
| 网络结构 | YOLOv8 结构来自 Ultralytics，按其 AGPL-3.0 许可；本队仅以其训练自有模型 |
| 训练数据 | 网球数据集来自 Roboflow 的第三方公开数据，其许可以来源页为准，使用前需按该许可核对 |

模型权重是本队训练的产物。涉及 Ultralytics 结构与 Roboflow 数据的许可条款以各自来源为准，本说明仅作归属标注。
