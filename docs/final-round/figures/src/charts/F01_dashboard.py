#!/usr/bin/env python3
"""F01_dashboard — cross-subsystem parity dashboard (StarryOS vs Linux).

Horizontal bars, one per benchmarked axis, showing a normalized "fraction of
parity" against Linux. Throughput axes are Starry/Linux; latency/time axes are
Linux/Starry — so in every row, higher is always better and a dashed line at
x=1.0 marks parity. Bars >=1.0 are teal (ahead/on-par), <1.0 are amber
(behind). Rows sourced from figures/data/F01.json; board-pending values carry
"abl": true and are drawn with a hatch + asterisk.

Usage: python3 figures/src/charts/F01_dashboard.py
Writes figures/out/F01_dark.png and figures/out/F01_light.png.
"""
import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

FIGURES_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(FIGURES_DIR.parent / "style"))
import style  # noqa: E402

DATA_PATH = FIGURES_DIR / "data" / "F01.json"
OUT_DIR = FIGURES_DIR / "out"

BAR_HEIGHT = 0.58
ABL_HATCH = "///"


def load_rows():
    with open(DATA_PATH, encoding="utf-8") as f:
        data = json.load(f)
    rows = sorted(data["rows"], key=lambda r: r["value"], reverse=True)
    return data, rows


def build(dark: bool):
    style.apply(dark=dark)
    data, rows = load_rows()
    n = len(rows)

    fg = style.INK if dark else "#1a2230"
    surface = style.NAVY if dark else "#FFFFFF"

    fig, ax = plt.subplots(figsize=(8.6, 5.4))

    y = np.arange(n)
    values = [r["value"] for r in rows]
    colors = [style.TEAL if r["value"] >= 1.0 else style.AMBER for r in rows]
    hatches = [ABL_HATCH if r.get("abl") else None for r in rows]

    for yi, val, col, hat, row in zip(y, values, colors, hatches, rows):
        ax.barh(
            yi,
            val,
            height=BAR_HEIGHT,
            color=col,
            alpha=0.55 if hat else 0.92,
            edgecolor=col,
            linewidth=1.2,
            hatch=hat,
            zorder=3,
        )

    # Parity line at x = data["parity_line"]
    parity = data.get("parity_line", 1.0)
    ax.axvline(parity, color=style.MUTED, linestyle=(0, (4, 3)), linewidth=1.4, zorder=2)
    ax.text(
        parity,
        n - 0.35,
        data.get("parity_label", "x=1.0"),
        color=style.MUTED,
        fontsize=12,
        ha="left",
        va="bottom",
    )

    # Value + tag labels at each bar end.
    x_max = max(values) * 1.32
    for yi, val, row, hat in zip(y, values, rows, hatches):
        star = " *" if row.get("abl") else ""
        label = f"{val:.2f}× · {row['tag']}{star}"
        label_x = val + x_max * 0.018
        ax.text(
            label_x,
            yi,
            label,
            color=fg,
            fontsize=13,
            ha="left",
            va="center",
            zorder=4,
        )

    ax.set_yticks(y)
    ax.set_yticklabels([r["label"] for r in rows], fontsize=13)
    ax.invert_yaxis()  # largest value on top, since rows are sorted descending

    ax.set_xlim(0, x_max)
    ax.set_xlabel(data.get("x_label", ""), fontsize=13, labelpad=8)
    ax.tick_params(axis="both", length=0)
    ax.grid(axis="x", alpha=0.18)
    ax.grid(axis="y", visible=False)
    ax.set_ylim(n - 0.5, -0.5)

    ax.set_title(data.get("title", ""), fontsize=20, fontweight="bold", pad=28, loc="left")
    if data.get("subtitle"):
        ax.text(
            0.0,
            1.045,
            data["subtitle"],
            transform=ax.transAxes,
            fontsize=13,
            color=style.MUTED,
            ha="left",
            va="bottom",
        )

    # Legend: status colors (teal/amber) + abl hatch.
    legend_handles = [
        plt.Rectangle((0, 0), 1, 1, facecolor=style.TEAL, edgecolor=style.TEAL, alpha=0.92, label="≥ 1.0 领先"),
        plt.Rectangle((0, 0), 1, 1, facecolor=style.AMBER, edgecolor=style.AMBER, alpha=0.92, label="<1.0 持平/落后"),
    ]
    legend = ax.legend(
        handles=legend_handles,
        loc="lower right",
        frameon=False,
        fontsize=12,
        labelcolor=fg,
        handlelength=1.3,
        handleheight=1.1,
        borderaxespad=0.4,
    )

    if data.get("footnote"):
        fig.text(
            0.01,
            0.01,
            data["footnote"],
            fontsize=11,
            color=style.MUTED,
            ha="left",
            va="bottom",
        )

    fig.tight_layout(rect=(0, 0.03, 1, 1))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    suffix = "dark" if dark else "light"
    out_path = OUT_DIR / f"F01_{suffix}.png"
    fig.savefig(out_path, dpi=200, facecolor=surface, bbox_inches="tight")
    plt.close(fig)
    return out_path


def main():
    dark_path = build(dark=True)
    light_path = build(dark=False)
    print(f"wrote {dark_path}")
    print(f"wrote {light_path}")


if __name__ == "__main__":
    main()
