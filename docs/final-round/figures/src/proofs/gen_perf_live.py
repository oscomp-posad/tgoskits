#!/usr/bin/env python3
"""Live board perf panel — real captures taken on the physical RK3588
(OrangePi-5-Plus) on 2026-08-17 via the board-control serial harness. The star
is `perf stat` showing all three PMUs (A55 / A76 / generic pmuv3) counting at
once with a real IPC; below are perf-validate highlights (exclude filter,
cross-cluster isolation, rdpmc, summary). Run from the repo root:

    python3 figures/src/proofs/gen_perf_live.py   ->  figures/out/Fperf_live.png

Source text: figures/src/proofs/board_perf_live_2026-08-17.txt
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.font_manager as fm
from matplotlib.patches import FancyBboxPatch

NAVY = "#0D1B2A"; PANEL = "#0f2233"; TEAL = "#2DD4BF"; AMBER = "#F59E0B"
INK = "#EEF3F8"; MUTED = "#8AA0B4"; GREEN = "#7BE38A"
for f in ["/System/Library/Fonts/Menlo.ttc",
          "/System/Library/Fonts/Supplemental/Songti.ttc",
          "/System/Library/Fonts/STHeiti Medium.ttc"]:
    try: fm.fontManager.addfont(f)
    except Exception: pass
plt.rcParams.update({"font.family": ["Menlo", "Songti SC"], "axes.unicode_minus": False})

P = TEAL
L = [
 ("root@starry:~# perf stat -e cycles,instructions,cache-references,cache-misses ./true", INK, True),
 ("      27,387,142   armv8_cortex_a55/cycles/            (91%)", INK, False),
 ("      27,374,447   armv8_cortex_a76/cycles/            (92%)   ← 大小核双 PMU 同时计数", GREEN, False),
 ("      27,364,487   armv8_pmuv3_0/cycles/               (99%)", INK, False),
 ("      33,678,116   armv8_cortex_a55/instructions   #  1.23  insn per cycle", INK, False),
 ("      33,690,203   armv8_cortex_a76/instructions   #  1.23  insn per cycle", INK, False),
 ("       9,520,167   armv8_cortex_a76/cache-references", INK, False),
 ("          93,449   armv8_cortex_a55/cache-misses   #  1.02% of all cache refs", INK, False),
 ("         0.0601 seconds time elapsed", MUTED, False),
 ("", INK, False),
 ("root@starry:~# perf-validate      # 真机 PMU / 大小核特性校验", INK, True),
 ("  CTR-EL   PASS  user 160000040  >>  kernel 35034      (exclude-bit 过滤生效)", GREEN, False),
 ("  CLU-1/2  PASS  A76-PMU-on-A55 → ENOENT · A55-PMU-on-A76 → ENOENT  (簇隔离)", GREEN, False),
 ("  RDPMC-2  PASS  用户态 rdpmc cap=1 width=64 读数与内核一致", GREEN, False),
 ("  SAMP/KPROBE PASS  硬件采样 2048、kprobe→SAMPLE 命中", GREEN, False),
 ("  online=8 · 4+4 双簇 · 核心 PMU / 大小核 / rdpmc / kprobe 均现场通过", GREEN, True),
]
fig, ax = plt.subplots(figsize=(11.4, 6.6))
fig.patch.set_facecolor(NAVY); ax.set_facecolor(NAVY); ax.axis("off")
ax.add_patch(FancyBboxPatch((0.008, 0.16), 0.984, 0.80,
             boxstyle="round,pad=0.006,rounding_size=0.02", transform=ax.transAxes,
             fc=PANEL, ec=MUTED, lw=1.0, zorder=0))
for i, c in enumerate(["#ff5f56", "#ffbd2e", "#27c93f"]):
    ax.add_patch(plt.Circle((0.035 + i * 0.022, 0.925), 0.009, transform=ax.transAxes, color=c, zorder=2))
ax.text(0.5, 0.925, "perf @ OrangePi-5-Plus (RK3588 · 4×A55 + 4×A76) — 真机现场采集",
        transform=ax.transAxes, ha="center", va="center", color=MUTED, fontsize=12)
y = 0.855; dy = (0.855 - 0.205) / (len(L) - 1)
for t, c, b in L:
    ax.text(0.03, y, t, transform=ax.transAxes, ha="left", va="center",
            color=c, fontsize=11.6, fontweight=("bold" if b else "normal"))
    y -= dy
ax.add_patch(FancyBboxPatch((0.008, 0.015), 0.984, 0.115,
             boxstyle="round,pad=0.004,rounding_size=0.02", transform=ax.transAxes,
             fc="#122e2a", ec=TEAL, lw=1.2, zorder=0))
ax.text(0.5, 0.087, "同一份未改上游 perf 6.6.0 · A55=type9 / A76=type10 双簇独立编程 · IPC 与 cache-miss 均为真实硅片计数",
        transform=ax.transAxes, ha="center", va="center", color=TEAL, fontsize=11.5, fontweight="bold")
ax.text(0.5, 0.042, "2026-08-17 经 board-control 串口在物理板上现场采集（perf stat 三 PMU 同时计数为本次实拍）",
        transform=ax.transAxes, ha="center", va="center", color=MUTED, fontsize=10)
# titleless: the deck/report frametitle supplies the title (avoids double-title + bottom overflow)
fig.savefig("figures/out/Fperf_live.png", dpi=200, facecolor=NAVY, bbox_inches="tight")
print("wrote figures/out/Fperf_live.png")
