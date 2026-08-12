#!/usr/bin/env python3
"""F05_residency: core-residency BEFORE/AFTER grouped-bar chart.

Shows the runtime-share of the 8 RK3588 CPU cores (cpu0-3 = A55 little
cluster, cpu4-7 = A76 big cluster) before and after the load-balancer /
capacity-aware placement fix: BEFORE is oversubscribed on the A76 big
cluster (cpu5-7) with the A55 little cluster near-idle; AFTER is spread
across all 8 cores.

Reads its data from figures/data/F05.json (all values currently
board-pending, flagged with "abl": true) and writes
figures/out/F05_dark.png + figures/out/F05_light.png.
"""

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.transforms import blended_transform_factory  # noqa: E402

FIGURES_DIR = Path(__file__).resolve().parents[2]
DATA_PATH = FIGURES_DIR / "data" / "F05.json"
OUT_DIR = FIGURES_DIR / "out"


def load_data() -> dict:
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def render(data: dict, dark: bool) -> Path:
    style.apply(dark=dark)
    fg = style.INK if dark else "#1a2230"

    cores = data["cores"]
    n = len(cores)
    x = np.arange(n)
    before = [c["before_pct"] for c in cores]
    after = [c["after_pct"] for c in cores]
    any_abl = data.get("abl") or any(c.get("abl") for c in cores)

    fig, ax = plt.subplots(figsize=(7.2, 4.6))

    bar_w = 0.34
    gap = 0.04
    ax.bar(
        x - bar_w / 2 - gap / 2, before, width=bar_w,
        color=style.MUTED, label=data["legend_zh"]["before"],
        zorder=3, linewidth=0,
    )
    ax.bar(
        x + bar_w / 2 + gap / 2, after, width=bar_w,
        color=style.TEAL, label=data["legend_zh"]["after"],
        zorder=3, linewidth=0,
    )

    # thin divider between the A55 (little) and A76 (big) clusters
    divider_x = 3.5
    ax.axvline(divider_x, color=style.MUTED, alpha=0.35, linewidth=1, zorder=1)

    # cluster labels anchored to bar x-position (data coords) but pinned to an
    # absolute row near the bottom of the figure (figure-fraction y), so they
    # never collide with the x-tick labels and never clip off the canvas.
    cluster_labels = data["cluster_labels_zh"]
    cluster_trans = blended_transform_factory(ax.transData, fig.transFigure)
    ax.text(
        1.5, 0.035, cluster_labels["A55"], transform=cluster_trans,
        ha="center", va="bottom", fontsize=9, color=style.MUTED, clip_on=False,
    )
    ax.text(
        5.5, 0.035, cluster_labels["A76"], transform=cluster_trans,
        ha="center", va="bottom", fontsize=9, color=style.MUTED, clip_on=False,
    )

    ax.set_xticks(x)
    ax.set_xticklabels([c["id"] for c in cores], fontsize=9)
    ax.set_xlim(-0.7, n - 0.3)
    ax.set_ylabel(data["ylabel_zh"], fontsize=10)
    ax.set_ylim(0, max(max(before), max(after)) * 1.22)

    # title + ablation-pending badge live in figure space (not axes space) on
    # two clearly separated rows, so a long badge string never runs into the
    # centered title regardless of figure width.
    fig.suptitle(data["title_zh"], x=0.54, y=0.965, fontsize=13, fontweight="bold", color=fg)

    # selective direct labels: only the extreme of each series tells the story
    max_before_i = int(np.argmax(before))
    ax.annotate(
        f"{before[max_before_i]}%",
        xy=(x[max_before_i] - bar_w / 2 - gap / 2, before[max_before_i]),
        xytext=(0, 4), textcoords="offset points",
        ha="center", va="bottom", fontsize=9,
    )
    max_after_i = int(np.argmax(after))
    ax.annotate(
        f"{after[max_after_i]}%",
        xy=(x[max_after_i] + bar_w / 2 + gap / 2, after[max_after_i]),
        xytext=(0, 4), textcoords="offset points",
        ha="center", va="bottom", fontsize=9,
    )

    ax.legend(loc="upper right", frameon=False, fontsize=9)

    if any_abl:
        fig.text(
            0.03, 0.895, data.get("abl_note_zh", "占位数据 · 板级消融实验待更新"),
            ha="left", va="top", fontsize=8, color=style.AMBER, style="italic",
        )

    ax.grid(axis="y", zorder=0)
    ax.grid(axis="x", visible=False)
    ax.tick_params(axis="x", length=0)
    fig.subplots_adjust(left=0.11, right=0.97, top=0.80, bottom=0.22)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    suffix = "dark" if dark else "light"
    out_path = OUT_DIR / f"F05_{suffix}.png"
    fig.savefig(out_path, dpi=180)
    plt.close(fig)
    return out_path


def main() -> None:
    data = load_data()
    dark_path = render(data, dark=True)
    light_path = render(data, dark=False)
    print(f"wrote {dark_path}")
    print(f"wrote {light_path}")


if __name__ == "__main__":
    main()
