# 面向边缘智能的 AIOS 设计与优化

> 在 Starry 上承载 RK3588 网球拾取机器人视觉流水线的实现与系统级优化

全国大学生计算机系统能力大赛 · 操作系统设计赛 · 功能挑战赛道

| 项 | 内容 |
|---|---|
| 赛题编号 | proj4（工程型，难度 A） |
| 目标平台 | OrangePi 5 Plus / RK3588 |
| 参赛队伍 | Posad |
| 参赛学校 | 清华大学 |
| 队　　员 | 洪世金、王乙凡、徐子航 |
| 开源协议 | 源代码 Apache-2.0，文档与答辩材料 CC BY-SA 4.0 |

本工作面向边缘智能场景下的具身智能应用，将一台基于 RK3588 的开源网球拾取机器人的操作系统，由基于 C 语言的 Linux 迁移至以 Rust 实现的 Starry。机器人由相机采集图像，经 YOLOv8 模型在片上 NPU 上完成目标检测，进而驱动机械臂与底盘完成拾球。机器人从相机感知到机械臂抓取的完整回路已在 Starry 上跑通，检测结果与 Linux 一致；在与相机输出一致的 480×640 输入下，端到端推理时延达到 11.75 ms，与 Linux 的 11.4 ms 处于同一水平。

## 提交材料

初赛的三项主要材料如下，其余说明文档见后文。

| 材料 | 入口 |
|---|---|
| 设计方案文档（初赛）| 📄 [`docs/设计方案（初赛）.pdf`](docs/设计方案（初赛）.pdf) |
| 进展汇报幻灯片 | 📑 [`slides/进展汇报（初赛）.pdf`](slides/进展汇报（初赛）.pdf) |
| 作品演示视频 | 🎬 [百度网盘](https://pan.baidu.com/s/1h1XYYQY2sgXvKzJpxdhcLw?pwd=tuc4)（提取码 `tuc4`），说明见 [`video/演示视频说明.md`](video/演示视频说明.md) |

## 决赛材料

| 材料 | 入口 |
|---|---|
| 技术报告（决赛，83 页）| 📄 [`docs/final-round/技术报告（决赛）.pdf`](docs/final-round/技术报告（决赛）.pdf) |
| 进展汇报（决赛，33 页）| 📑 [`docs/final-round/进展汇报（决赛）.pdf`](docs/final-round/进展汇报（决赛）.pdf) |
| 进展汇报（决赛，可放映 PPTX）| 📊 [`docs/final-round/进展汇报（决赛）.pptx`](docs/final-round/进展汇报（决赛）.pptx) |
| 答辩视频（决赛，约 8 分钟）| 🎬 [`docs/final-round/答辩视频.mp4`](docs/final-round/答辩视频.mp4) |
| 决赛材料目录（LaTeX 源码、数据、图）| 📁 [`docs/final-round/`](docs/final-round/) |

## 基础版本

本工作在若干开源项目的基础上展开，相关依赖在首次提交中即予标注。操作系统内核基于 rcore-os/tgoskits 的 dev 分支，该仓库集成了 Starry 与 ArceOS 的组件；机器人应用移植自 pengzechen/aka-rk3588，并改造为一个 tgoskits 应用；模型由 rknn-toolkit2 转换为 NPU 所用的 RKNN 格式。

| 基础版本 | 仓库 | 基线 |
|---|---|---|
| 操作系统内核 | rcore-os/tgoskits @ `dev` | commit `73409e079`（2026-06-22，“chore: release (#1263)”）|
| 机器人应用 | pengzechen/aka-rk3588 | 见 [`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) |
| 模型转换工具 | airockchip/rknn-toolkit2 | 转换为 RKNN 格式 |

## 完成情况

本工作沿评审的三个阶段推进，先在开发板上打通推理链路，再补齐机器人本体的感知与执行以完成实际拾球，最后以 Linux 为基线开展性能优化。

| 阶段 | 状态 | 说明 |
|---|---|---|
| 一　启动并跑通 NPU 推理 | ✅ | 块设备完成路径容错使开发板稳定挂载，首次推理结果与 Linux 完全一致 |
| 二　接齐本体外设并实际拾球 | ✅ | USB 串口驱动连接机械臂控制器，相机经既有 USB 栈取流，完整拾球回路跑通 |
| 三　以 Linux 为基线的性能优化 | ✅ | 端到端推理由 202.9 ms 收敛至 65.1 ms（640×640），480×640 下达到 11.75 ms |
| 　　启动、内存与能耗的多维对照 | 进行中 | 常驻内存更省（约 19 比 31 MB），模型冷启动更慢（约 3.6 比 0.8 s，列为优化方向），整机功耗待板上采集 |

初赛要求的各项提交物及其在本仓库中的位置如下。

| 提交物 | 位置 | 状态 |
|---|---|---|
| 设计方案等开发文档 | [`docs/设计方案（初赛）.pdf`](docs/设计方案（初赛）.pdf) 及 `docs/` 下各说明 | ✅ |
| 项目源代码 | 本仓库内核树、上游合并请求与 `apps/` 应用 | ✅ |
| 功能、性能与创新性分析（含类似项目对比）| 报告第三节与 [`docs/与类似项目对比分析.md`](docs/与类似项目对比分析.md) | ✅ |
| 进展汇报幻灯片（含分工、进度与 AI 使用）| [`slides/进展汇报（初赛）.pdf`](slides/进展汇报（初赛）.pdf) | ✅ |
| 演示视频 | [`video/演示视频说明.md`](video/演示视频说明.md) | ✅ |
| 多次真实提交记录 | 本仓库提交历史与上游合并请求 | ✅ |

## 关键结果

两侧采用同一份遵循 Linux ABI 的可执行文件，加载相同的推理运行时与模型权重，使观察到的差异尽可能归结于操作系统本身。最初配置下，采集、解码、缩放与推理全部串行于一个 A55 小核，端到端时延约为 Linux 的六倍。将计算量最大的推理放置到 A76 大核后降至三分之一，再以整数路径取回输出并配合与相机一致的 480×640 输入，端到端推理 p50 降至 11.75 ms，与 Linux 在同一输入尺寸下的 11.4 ms 持平。640×640 与 480×640 是同一模型的两种输入尺寸，各组数据应与对应尺寸下的 Linux 基线相比。

| 输入尺寸 | Starry 最初 | 关键优化后 | Linux 基线 |
|---|---|---|---|
| 640×640 | 202.9 | 65.1（推理绑定 A76）| 25.8 |
| 480×640 | 30.6 | 11.75（整数输出与大核放置）| 11.4 |

（端到端推理 p50，单位 ms）

在此基础上再以 RGA 承担解码与缩放并直接写入 NPU 的输入缓冲，自相机采集到控制指令就绪的整条回路 p50 为 9.65 ms，受相机约 28 fps 的供帧速率限制。

进一步接入 JPU 硬件解码后，完整的 MJPEG→JPU→RGA→NPU 零拷贝流水线在 Linux 与 Starry 上均已跑通。在各自默认调频策略下，Starry 的端到端中位时延更低（9.06 比 13.89 ms），但这源于 Starry 把加速器固定在最高频而不降频，而非推理本身更快；在同一时钟下两侧的 NPU 推理本就相当，与上表 11.75 和 11.4 的持平一致，把 Linux 切到 performance 调频策略即可消除该差距。不降频的代价体现在尾部，Starry 偶有数百毫秒的停顿（最坏 487 比 22 ms）并丢弃约一成的帧，模型冷启动也更慢，这些都列为后续的调度与驱动优化方向；常驻内存则反而更省（约 19 比 31 MB），两侧检测精度一致。

## 增量贡献

本队在上述基础上的增量贡献见下表，其中多项已作为合并请求提交至上游仓库 rcore-os/tgoskits，其余以分支形式保留待提交。已合并的改动随基础版本一同位于本仓库，开放中与待提交的改动分别见对应的合并请求与分支。

| 贡献 | 位置 | 上游提交 |
|---|---|---|
| 硬件 PMU perf，按任务到大小核的原生 perf | StarryOS perf 子系统 | [PR #1395](https://github.com/rcore-os/tgoskits/pull/1395)，已合并 |
| USB 串口驱动，机械臂控制器 `/dev/ttyUSB0` | `drivers/usb/` CP210x | [PR #1378](https://github.com/rcore-os/tgoskits/pull/1378)，已合并 |
| reboot 系统调用，加速 Linux 与 Starry 的切换 | StarryOS 系统调用层 | [PR #1358](https://github.com/rcore-os/tgoskits/pull/1358)，已合并 |
| rknpu DRM 整合与 GEM 缓冲修复 | `os/StarryOS/.../dev/card1.rs`、`drm.rs` | [PR #1351](https://github.com/rcore-os/tgoskits/pull/1351)、[#1364](https://github.com/rcore-os/tgoskits/pull/1364)，已合并 |
| 图形与显示内核能力，使 Weston 合成器在 Starry 上运行 | DRM/KMS、evdev 输入、AF_UNIX/AF_NETLINK 套接字、memfd 封印、epoll 与 timerfd | [PR #506](https://github.com/rcore-os/tgoskits/pull/506)、[#509](https://github.com/rcore-os/tgoskits/pull/509)、[#513](https://github.com/rcore-os/tgoskits/pull/513)、[#514](https://github.com/rcore-os/tgoskits/pull/514) 等（#503 至 #516、[#1160](https://github.com/rcore-os/tgoskits/pull/1160)），已合并 |
| RGA 驱动，`/dev/rga`，对齐 librga 接口 | `drivers/` rockchip-rga | [PR #1388](https://github.com/rcore-os/tgoskits/pull/1388)，开放中 |
| JPU 驱动，`/dev/mpp_service`，对齐 MPP 接口 | `drivers/vpu/rockchip-jpeg/` | [PR #1456](https://github.com/rcore-os/tgoskits/pull/1456)，开放中 |
| 块设备完成路径容错，中断超时回退轮询 | `os/arceos/modules/axfs-ng/` | 待提交（commit `5c36d90e3`）|
| PWM 驱动，机械臂与底盘电机 | 机械臂与底盘电机驱动 | 待提交 |
| profile-rknpu 计时特性，NPU ioctl 分阶段计数 | `card1.rs`（`RKNPU_KPROFILE`）| 待提交 |
| 大小核负载均衡器原型，架构感知调度的基础 | axtask 调度 | 待提交 |
| 剖析工具链与基准应用 | `apps/` 与 `scripts/profile/` | 随应用提交 |

## 后续工作

系统层面有几处仍可推进。架构感知的调度将依据各核算力自动放置用户态与内核态的线程，免去手动绑核。NPU 的三核可独立工作，而当前的串行点在于设备锁在整个推理期间被持有，一个以每核队列与中断组织的并发完成路径，是进一步提升推理吞吐的结构前提。本队为运行 Weston 合成器而实现的一组图形与显示内核能力，亦开出一条面向用户侧的方向，在开发板上原生呈现相机画面、检测框与机器人状态的监控界面，并把 Weston 的图层合成由 CPU 改交视觉流水线已用到的同一块 RGA 引擎承担。

机器人应用层面规划了以下方向。

- 引入 ACT 策略（动作分块的模仿学习），让机器人学习并执行更复杂的动作。
- 加入语音输入，使任务可由语音指令触发。
- 设计更稳健的抓取策略，把抓球的成功率做到接近百分之百。
- 支持里程计，使机器人始终记得回收桶的位置，从而把球准确放入。

## 其它文档

`docs/` 目录下的其余说明文档如下。

| 类别 | 文件 |
|---|---|
| 快速运行手册 | [`docs/快速运行手册.md`](docs/快速运行手册.md) |
| 与类似项目对比分析 | [`docs/与类似项目对比分析.md`](docs/与类似项目对比分析.md) |
| 开发问题与解决记录 | [`docs/问题与解决记录.md`](docs/问题与解决记录.md) |
| 基础版本与增量贡献 | [`docs/基础版本与增量贡献.md`](docs/基础版本与增量贡献.md) |
| AI 使用说明 | [`docs/AI使用说明.md`](docs/AI使用说明.md) |

## 构建与运行

完整的工具链、编译、刷板、U-Boot 引导与基准运行步骤见 [`docs/快速运行手册.md`](docs/快速运行手册.md)。在无开发板时，可经容器中的 QEMU 运行评测。

```bash
# 工具链 nightly-2026-04-27（见 rust-toolchain.toml）
cargo xtask starry rootfs --arch aarch64   # 准备根文件系统
cargo xtask starry qemu  --arch aarch64    # 在 QEMU 上运行
```

开发板上的机器人流水线基准经 `scripts/profile/` 下的脚本运行，剖析数据以纯标准库渲染为图。

## 开源协议

源代码以 Apache License 2.0 授权，沿用上游 tgoskits 的协议，见 [`LICENSE`](LICENSE)。文档与答辩材料以 Creative Commons Attribution-ShareAlike 4.0 授权，涵盖 `docs/`、`slides/` 与 `video/`，见 [`LICENSE-docs`](LICENSE-docs)。

## AI 使用

本工作系统性地使用了 AI 工具辅助开发，并把如何让 AI 产出可靠的操作系统代码作为一项方法上的产出加以总结。所用工具与大模型、使用场景、AI 成果范围、人工核验与修改，以及交互记录的位置，见 [`docs/AI使用说明.md`](docs/AI使用说明.md) 与设计方案文档对应章节。
