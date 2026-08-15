#!/usr/bin/env python3
"""F01_dashboard — cross-subsystem before/after parity dashboard (StarryOS vs Linux).

One row per benchmarked axis, drawn as a before -> after "dumbbell": a hollow dot
marks StarryOS *before* our work, an arrow runs to a filled dot marking StarryOS
*after*, both expressed as a normalized fraction of Linux. Throughput axes are
Starry/Linux; latency/time axes are Linux/Starry — so in every row higher is
better and the dashed line at x=1.0 marks Linux parity. The after dot is teal
when it reaches or passes parity, amber when a gap remains. Showing the before
value reframes each row from "behind Linux" to "how far the gap was closed."

Rows sourced from figures/data/F01.json; board-pending values carry "abl": true
and are drawn with a hollow (unfilled) after dot + asterisk.

Usage: python3 figures/src/charts/F01_dashboard.py
Writes figures/out/F01_dark.png and figures/out/F01_light.png.
"""
import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D

FIGURES_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(FIGURES_DIR.parent / "style"))
import style  # noqa: E402

DATA_PATH = FIGURES_DIR / "data" / "F01.json"
OUT_DIR = FIGURES_DIR / "out"


def load_rows():
    with open(DATA_PATH, encoding="utf-8") as f:
        data = json.load(f)
    rows = sorted(data["rows"], key=lambda r: r["value"], reverse=True)
    return data, rows


def build(dark: bool, show_title: bool = True):
    style.apply(dark=dark)
    data, rows = load_rows()
    n = len(rows)

    fg = style.INK if dark else "#1a2230"
    surface = style.NAVY if dark else "#FFFFFF"

    # One row per metric with generous vertical spacing (~0.62in per row).
    fig, ax = plt.subplots(figsize=(9.0, 6.8))

    y = np.arange(n)
    afters = [r["value"] for r in rows]
    befores = [r.get("before", r["value"]) for r in rows]
    x_max = max(afters) * 1.30

    for yi, row in zip(y, rows):
        b = row.get("before", row["value"])
        a = row["value"]
        col = style.TEAL if a >= 1.0 else style.AMBER
        pending = bool(row.get("abl"))

        # before -> after arrow (destination-colored so the eye lands on "after").
        ax.annotate(
            "",
            xy=(a, yi),
            xytext=(b, yi),
            arrowprops=dict(
                arrowstyle="-|>",
                color=col,
                lw=2.4,
                alpha=0.85,
                shrinkA=7,
                shrinkB=8,
                mutation_scale=17,
            ),
            zorder=2,
        )
        # before dot: hollow, muted — the starting point.
        ax.scatter(
            [b], [yi],
            s=92,
            facecolors=surface,
            edgecolors=style.MUTED,
            linewidths=1.8,
            zorder=4,
        )
        # after dot: filled (hollow if board-pending) — the current state.
        ax.scatter(
            [a], [yi],
            s=150,
            facecolors=surface if pending else col,
            edgecolors=col,
            linewidths=2.0,
            zorder=5,
        )

        # after label: bold, to the right of the after dot.
        star = " *" if pending else ""
        ax.text(
            a + x_max * 0.020,
            yi,
            f"{a:.2f}× · {row['tag']}{star}",
            color=fg,
            fontsize=13,
            fontweight="bold",
            ha="left",
            va="center",
            zorder=6,
        )
        # before label: small, muted, floating just above the before dot.
        ax.text(
            b,
            yi - 0.34,
            f"{b:.2f}×",
            color=style.MUTED,
            fontsize=10.5,
            ha="center",
            va="bottom",
            zorder=6,
        )

    # Linux parity line at x = data["parity_line"].
    parity = data.get("parity_line", 1.0)
    ax.axvline(parity, color=style.MUTED, linestyle=(0, (4, 3)), linewidth=1.4, zorder=1)
    ax.text(
        parity + x_max * 0.006,
        -0.72,
        data.get("parity_label", "x=1.0"),
        color=style.MUTED,
        fontsize=11.5,
        ha="left",
        va="bottom",
    )

    ax.set_yticks(y)
    ax.set_yticklabels([r["label"] for r in rows], fontsize=13)

    ax.set_xlim(0, x_max)
    ax.set_xlabel(data.get("x_label", ""), fontsize=13, labelpad=8)
    ax.tick_params(axis="both", length=0)
    ax.grid(axis="x", alpha=0.16)
    ax.grid(axis="y", visible=False)
    ax.set_ylim(n - 0.4, -0.9)  # row 0 (largest after) on top

    if show_title:
        ax.set_title(data.get("title", ""), fontsize=20, fontweight="bold", pad=30, loc="left")
        if data.get("subtitle"):
            ax.text(
                0.0,
                1.045,
                data["subtitle"],
                transform=ax.transAxes,
                fontsize=12.5,
                color=style.MUTED,
                ha="left",
                va="bottom",
            )

    legend_handles = [
        Line2D([0], [0], marker="o", linestyle="none", markersize=9,
               markerfacecolor=surface, markeredgecolor=style.MUTED,
               markeredgewidth=1.8, label="优化前"),
        Line2D([0], [0], marker="o", linestyle="none", markersize=10,
               markerfacecolor=style.TEAL, markeredgecolor=style.TEAL,
               label="优化后·达到或超过 Linux"),
        Line2D([0], [0], marker="o", linestyle="none", markersize=10,
               markerfacecolor=style.AMBER, markeredgecolor=style.AMBER,
               label="优化后·仍有差距"),
        Line2D([0], [0], linestyle=(0, (4, 3)), color=style.MUTED,
               linewidth=1.4, label="Linux 基线"),
    ]
    ax.legend(
        handles=legend_handles,
        loc="lower right",
        frameon=False,
        fontsize=11.5,
        labelcolor=fg,
        handlelength=1.5,
        borderaxespad=0.6,
        labelspacing=0.7,
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
    if not show_title:
        suffix = f"deck_{suffix}"  # titleless variant: the Beamer frametitle supplies the title
    out_path = OUT_DIR / f"F01_{suffix}.png"
    fig.savefig(out_path, dpi=200, facecolor=surface, bbox_inches="tight")
    plt.close(fig)
    return out_path


def main():
    dark_path = build(dark=True)
    light_path = build(dark=False)
    deck_path = build(dark=True, show_title=False)
    print(f"wrote {dark_path}")
    print(f"wrote {light_path}")
    print(f"wrote {deck_path}")


if __name__ == "__main__":
    main()
