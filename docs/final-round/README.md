# 决赛材料（Final Round）

Team Posad · 清华大学 · 项目「面向边缘智能的 AIOS 设计与优化」

本目录收录全国大学生操作系统竞赛**决赛**的技术报告与进展汇报，覆盖约一个月在 RK3588（OrangePi-5-Plus）上将网球拾取机器人的操作系统由 Linux 迁移至 StarryOS、并令整套 StarryOS 在通用负载上追平 Linux 的工作。

全部成果以原子 PR 回馈上游 [`rcore-os/tgoskits`](https://github.com/rcore-os/tgoskits)：自初赛至今，本队三名成员累计提交 122 个 PR，其中 **90 个已合并、19 个在审**（决赛期新增 24 个已合并 + 19 个在审），覆盖系统调用/进程/信号、调度与大小核、内存与分页、观测 perf、RGA/JPU/NPU 与原生显示驱动、网络、虚拟化与板级；另自建 QEMU RKNPU 功能级设备模型，全部经真机验证。

## 交付物

| 文件 | 说明 |
|------|------|
| [`技术报告（决赛）.pdf`](技术报告（决赛）.pdf) | 技术报告（83 页，ctexart） |
| [`进展汇报（决赛）.pdf`](进展汇报（决赛）.pdf) | 进展汇报幻灯片（33 页，ctexbeamer） |
| [`进展汇报（决赛）.pptx`](进展汇报（决赛）.pptx) | 进展汇报（可放映 PPTX，演示 GIF 就地播放） |
| [`答辩视频.mp4`](答辩视频.mp4) | 决赛答辩视频（约 8 分钟，1080p60） |

本目录即决赛材料的统一存放位置：技术报告、进展汇报（PDF/PPTX）与答辩视频均在此处。LaTeX 源码见本目录 [`report/`](report/) 与 [`slides/`](slides/)，构建方式见下文「从源码构建」。初赛进展汇报见 [`slides/进展汇报（初赛）.pdf`](../../slides/进展汇报（初赛）.pdf)，初赛设计方案见 [`docs/设计方案（初赛）.pdf`](../设计方案（初赛）.pdf)。

## 队员分工

- **洪世金（Joseph Joshua Anggita，队长）**：内核性能、内存与驱动的主体工作——perf 观测、调度器与大小核、频率与电压 DVFS、内存与 THP、页表与 TLB 正确性、RGA 与 JPU 驱动、原生显示栈、sysbench 与整机对比；报告撰写与整体协调。
- **王乙凡**：功能级仿真——QEMU RKNPU 功能级模型；机器人拾球应用（与徐子航共同）。
- **徐子航**：板级使能——板级 I/O 与网络、UVC 相机、reboot 系统调用；机器人拾球应用（与王乙凡共同）。

## 主要结果（RK3588 整机实测，3 次重复取中位数，对照同机同频 Linux 6.1.43）

| 指标 | StarryOS 优化前 → 后 | Linux | 判定 |
|------|----------------------|-------|------|
| sysbench cpu 8 线程 | 160 → 4987 ev/s | 5222 | 持平 0.96× |
| getpid 系统调用入口 | 1200 → 431 ns | 167 | 收窄至 2.6× |
| THP first-touch | 0.8–1.3 → 0.030 s | 0.086 | 领先 2.87× |
| schbench m1t4 RPS | 231 → 339 | 199 | 反超 1.71× |
| 启动 TTFI | 19.22 → 9.83 s | 11.28 | 领先 |
| 内存写带宽 8 线程 | 15040 → 34330 MiB/s | 56046 | 收窄至 0.61×（DDR 变频固件外因） |

## 从源码构建

工具链为 [tectonic](https://tectonic-typesetting.github.io/)（自动下载 ctex/beamer 依赖），中文字体为 Songti SC（正文）+ Heiti SC（标题）。

```sh
# 技术报告
cd report && tectonic main.tex --outdir build

# 进展汇报
cd slides && tectonic deck.tex --outdir build
```

## 目录结构

```
report/       技术报告 LaTeX 源（main.tex + preamble.tex + sections/）
slides/       进展汇报 LaTeX 源（deck.tex + theme/）
data/         numbers.tex —— 全部数字的唯一权威表，正文仅引用宏名
style/        共享调色板与 matplotlib 样式
figures/      渲染图不随仓库分发（已嵌入 PDF）；由 src/*.py + data/*.json 重新生成
lint.sh       字号/口径/调色板一致性检查
```

所有性能数字集中于 `data/numbers.tex`，正文与幻灯片仅引用宏名以保持一致；板级待测项以 `\abl{}` 标出。
