#!/usr/bin/env python3
"""Finfer_ladder — end-to-end inference latency ladders, ONE PANEL PER INPUT SIZE.

The two input sizes are incommensurable (different model scale, anchor count,
post-processing, and their own Linux baselines), so they must never share a
curve or a y-axis. This renders two independent panels, each a descending bar
ladder from the StarryOS default configuration down through the stacked
optimizations, with that size's own dashed Linux reference line.

Numbers mirror data/numbers.tex and section 5 prose:
  640x640: 162.7 -> 55.2 ms (A76 bind), Linux 25.8 ms
  480x640: 30.6 -> 11.75 (native+ReLU+int8 out) -> 9.65 ms (+RGA), Linux 11.4 ms

Writes figures/out/Finfer_dark.png and figures/out/Finfer_light.png.
"""
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style

OUT = Path("/Users/jsph273/Desktop/Code/optimization-efforts-final/figures/out")

# (label, value_ms, color)
PANELS = [
    {
        "title": "640×640 重型检测（对齐通用口径）",
        "linux": 25.8,
        "bars": [
            ("默认\n单核 A55", 162.7, style.MUTED),
            ("绑定 A76 大核", 55.2, style.TEAL),
        ],
    },
    {
        "title": "480×640 取球单类（相机原生分辨率）",
        "linux": 11.4,
        "bars": [
            ("默认", 30.6, style.MUTED),
            ("原生分辨率\n+ReLU+整数+A76绑定", 11.75, style.TEAL),
            ("+ RGA 零拷贝\n（逐帧命令）", 9.65, style.TEAL),
        ],
    },
]


def render(dark):
    style.apply(dark=dark)
    fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.6))
    for ax, panel in zip(axes, PANELS):
        labels = [b[0] for b in panel["bars"]]
        vals = [b[1] for b in panel["bars"]]
        cols = [b[2] for b in panel["bars"]]
        x = range(len(vals))
        bars = ax.bar(x, vals, color=cols, width=0.62, zorder=3)
        for xi, v in zip(x, vals):
            ax.text(
                xi,
                v + max(vals) * 0.02,
                f"{v:g}",
                ha="center",
                va="bottom",
                fontsize=12,
                fontweight="bold",
                color=style.INK if dark else "#1a2230",
            )
        lin = panel["linux"]
        ax.axhline(lin, ls="--", lw=1.4, color=style.AMBER, zorder=2)
        ax.text(
            len(vals) - 0.5,
            lin + max(vals) * 0.02,
            f"Linux {lin:g} ms",
            ha="right",
            va="bottom",
            fontsize=10.5,
            color=style.AMBER,
        )
        ax.set_xticks(list(x))
        ax.set_xticklabels(labels, fontsize=10.5)
        ax.set_ylim(0, max(vals) * 1.18)
        ax.set_ylabel("端到端时延（ms，越低越好）", fontsize=11)
        ax.set_title(panel["title"], fontsize=12.5, pad=10)
        ax.grid(axis="x", visible=False)
    fig.suptitle(
        "端到端时延阶梯：两种输入尺寸各自与同尺寸 Linux 基线比较",
        fontsize=14.5,
        fontweight="bold",
        y=1.02,
    )
    fig.tight_layout()
    suffix = "dark" if dark else "light"
    fig.savefig(OUT / f"Finfer_{suffix}.png", dpi=200, bbox_inches="tight")
    plt.close(fig)


if __name__ == "__main__":
    render(True)
    render(False)
    print("wrote Finfer_dark.png + Finfer_light.png")
