#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""F10_dvfs — DVFS per-core parity.

Grouped bar chart: StarryOS vs Linux sysbench events/s, one group per CPU
cluster (A55 / A76), plus a small annotation of the per-cluster OPP
frequency ceiling. Reads figures/data/F10.json (single source of truth for
the numbers); writes figures/out/F10_dark.png and figures/out/F10_light.png.

Board-pending values are flagged "abl": true in the JSON and rendered with a
hatch texture + a dagger marker + footnote, so the placeholder is visually
distinguishable from measured numbers without overloading series color.
"""
import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

HERE = Path(__file__).resolve()
FIGURES_DIR = HERE.parents[2]
DATA_PATH = FIGURES_DIR / "data" / "F10.json"
OUT_DIR = FIGURES_DIR / "out"


def load_data() -> dict:
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def render(dark: bool, data: dict, out_path: Path) -> None:
    style.apply(dark=dark)

    series_order = data["series_order"]
    series_labels = data["series_labels_cn"]
    series_colors = {series_order[0]: style.TEAL, series_order[1]: style.AMBER}
    clusters = data["clusters"]

    fg = style.INK if dark else "#1a2230"
    muted = style.MUTED

    fig, ax = plt.subplots(figsize=(7.6, 4.3), dpi=200)

    x = np.arange(len(clusters), dtype=float)
    bar_w = 0.32
    gap = 0.05
    offsets = {series_order[0]: -(bar_w / 2 + gap / 2), series_order[1]: (bar_w / 2 + gap / 2)}

    all_values = [c["series"][s]["value"] for c in clusters for s in series_order]
    ymax = max(all_values) * 1.28

    has_pending = False
    for si, s in enumerate(series_order):
        xs = x + offsets[s]
        heights = [c["series"][s]["value"] for c in clusters]
        abl_flags = [c["series"][s].get("abl", False) for c in clusters]
        color = series_colors[s]

        for xi, h, is_abl in zip(xs, heights, abl_flags):
            if is_abl:
                has_pending = True
                bar = ax.bar(
                    xi, h, width=bar_w, color=color, alpha=0.55,
                    hatch="////", edgecolor=muted, linewidth=0.8, zorder=3,
                )
                label = f"{h:g}†"
            else:
                bar = ax.bar(
                    xi, h, width=bar_w, color=color, alpha=0.95,
                    edgecolor="none", linewidth=0, zorder=3,
                )
                label = f"{h:g}"
            ax.text(
                xi, h + ymax * 0.018, label, ha="center", va="bottom",
                fontsize=10, color=fg, zorder=4,
            )

    # cluster labels + thin baseline
    ax.set_xticks(x)
    ax.set_xticklabels([c["label_cn"] for c in clusters], fontsize=11.5)
    ax.set_ylim(0, ymax)
    ax.set_ylabel(data["unit_cn"], fontsize=10.5, color=muted)
    ax.tick_params(axis="both", length=0)
    ax.grid(axis="y", zorder=0)
    ax.grid(axis="x", visible=False)
    ax.axhline(0, color=muted, linewidth=0.8, zorder=2)
    ax.set_xlim(-0.65, len(clusters) - 1 + 0.65)

    # legend: series identity + pending-texture meaning
    handles = [
        plt.Rectangle((0, 0), 1, 1, facecolor=series_colors[s], edgecolor="none", label=series_labels[s])
        for s in series_order
    ]
    if has_pending:
        handles.append(
            plt.Rectangle(
                (0, 0), 1, 1, facecolor=muted, alpha=0.55, hatch="////",
                edgecolor=muted, linewidth=0.8, label="† 板级消融待验证",
            )
        )
    legend = ax.legend(
        handles=handles, loc="upper left", frameon=False, fontsize=9.5,
        handlelength=1.3, handleheight=1.1, borderaxespad=0.2,
    )
    for text in legend.get_texts():
        text.set_color(fg)

    # small OPP-ceiling annotation
    ax.text(
        0.985, 0.965, data["opp_annotation_cn"],
        transform=ax.transAxes, ha="right", va="top", fontsize=9,
        color=muted, zorder=5,
    )

    # title / subtitle
    fig.suptitle(data["title_cn"], x=0.06, y=0.975, ha="left", fontsize=15, fontweight="bold", color=fg)
    ax.set_title(data["subtitle_cn"], loc="left", fontsize=10, color=muted, pad=14)

    # footnote for the pending placeholder
    if has_pending:
        fig.text(0.06, 0.015, data["pending_note_cn"], fontsize=8.5, color=muted, ha="left")

    fig.tight_layout(rect=(0, 0.03, 1, 0.94))
    fig.savefig(out_path, dpi=200)
    plt.close(fig)
    print(f"wrote {out_path}")


def main() -> None:
    data = load_data()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    render(dark=True, data=data, out_path=OUT_DIR / "F10_dark.png")
    render(dark=False, data=data, out_path=OUT_DIR / "F10_light.png")


if __name__ == "__main__":
    main()
