#!/usr/bin/env python3
"""Fstage_latency — per-stage latency of the three dominant inference stages,
before vs after binding the inference thread to the A76 big cluster (640x640).

Finfer_ladder shows only end-to-end totals; this decomposes WHERE the
milliseconds sit. On the starved single A55 core the whole 162.7 ms pipeline is
dominated by three CPU-side stages; binding to an idle A76 core deflates all
three, taking the end-to-end to 55.2 ms. Every number is measured p50 from
section 5.4 prose:
  run          71.1 -> 24.5 ms
  letterbox    75.5 -> 26.4 ms
  outputs_get  14.3 ->  2.7 ms

Writes figures/out/Fstage_dark.png and figures/out/Fstage_light.png.
"""
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style

OUT = Path("/Users/jsph273/Desktop/Code/optimization-efforts-final/figures/out")

# (stage label, default value on single A55, value after A76 bind)
STAGES = [
    ("run\nNPU 前向", 71.1, 24.5),
    ("letterbox\n缩放补边", 75.5, 26.4),
    ("outputs_get\n取回重排", 14.3, 2.7),
]


def render(dark):
    style.apply(dark=dark)
    fig, ax = plt.subplots(figsize=(9.6, 5.2))
    labels = [s[0] for s in STAGES]
    defaults = [s[1] for s in STAGES]
    bound = [s[2] for s in STAGES]
    x = range(len(STAGES))
    w = 0.38
    ink = style.INK if dark else "#1a2230"

    ax.bar([xi - w / 2 for xi in x], defaults, width=w, color=style.MUTED,
           zorder=3, label="默认（单核 A55）")
    ax.bar([xi + w / 2 for xi in x], bound, width=w, color=style.TEAL,
           zorder=3, label="绑定 A76 大核")

    ymax = max(defaults)
    for xi, (dv, bv) in zip(x, zip(defaults, bound)):
        ax.text(xi - w / 2, dv + ymax * 0.015, f"{dv:g}", ha="center",
                va="bottom", fontsize=11, fontweight="bold", color=ink)
        ax.text(xi + w / 2, bv + ymax * 0.015, f"{bv:g}", ha="center",
                va="bottom", fontsize=11, fontweight="bold", color=style.TEAL)
        # reduction factor hugging this group's taller (default) bar
        ax.text(xi, dv + ymax * 0.06, f"×{dv / bv:.1f}", ha="center",
                va="bottom", fontsize=12.5, fontweight="bold",
                color=style.AMBER)

    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylim(0, ymax * 1.20)
    ax.set_ylabel("单阶段 p50 时延（ms，越低越好）", fontsize=11.5)
    ax.set_title(
        "绑定 A76 大核前后：推理链路三大阶段时延（640×640）",
        fontsize=14, fontweight="bold", pad=26,
    )
    ax.grid(axis="x", visible=False)
    ax.legend(loc="upper right", fontsize=10.5, frameon=False)
    ax.text(
        0.5, 1.005,
        "这三个 CPU 侧阶段主导端到端时延；绑核把整链 162.7 → 55.2 ms",
        transform=ax.transAxes, ha="center", va="bottom",
        fontsize=10.5, color=style.MUTED,
    )
    fig.tight_layout()
    suffix = "dark" if dark else "light"
    fig.savefig(OUT / f"Fstage_{suffix}.png", dpi=200, bbox_inches="tight")
    plt.close(fig)


if __name__ == "__main__":
    render(True)
    render(False)
    print("wrote Fstage_dark.png + Fstage_light.png")
