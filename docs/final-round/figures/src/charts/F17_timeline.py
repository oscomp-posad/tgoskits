#!/usr/bin/env python3
"""F17_timeline — decision-round development timeline (7 月 -> 8 月).

Horizontal timeline: one thin baseline spanning the sprint, with milestone
stems alternating above/below (perf 多核, TTFI, QEMU NPU, DVFS, 占用调度,
THP, sysbench parity, 正确性 campaign). Milestones are colored by category
(style.TEAL = performance/scheduling parity, style.AMBER = systems
infrastructure, style.MUTED = tooling/verification). Data lives in
figures/data/F17.json; any milestone flagged "abl": true would be drawn
with a dashed stem + lower alpha + a trailing "*" (none are pending here —
dates are locked, no ablation dependency).

Usage: python3 figures/src/charts/F17_timeline.py
Writes figures/out/F17_dark.png and figures/out/F17_light.png.
"""

import json
import sys
from datetime import datetime, timedelta
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt

FIGURES_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

DATA_PATH = FIGURES_DIR / "data" / "F17.json"
OUT_DIR = FIGURES_DIR / "out"

STEM_H = 0.95
TITLE_GAP_PT = 7
DETAIL_GAP_PT = 24


def load_data():
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def _d(s: str) -> datetime:
    return datetime.strptime(s, "%Y-%m-%d")


def _mondays_between(start: datetime, end: datetime):
    d = start + timedelta(days=(7 - start.weekday()) % 7)
    while d <= end:
        yield d
        d += timedelta(days=7)


def _month_labels_between(start: datetime, end: datetime):
    """Yield (label_x, month, is_boundary) for every month touched by the range.

    The first (partial) month is labeled at range start with no separator;
    every subsequent month start gets both a separator line and a label.
    """
    yield (start, start.month, False)
    y, m = start.year, start.month
    while True:
        m += 1
        if m > 12:
            m = 1
            y += 1
        cand = datetime(y, m, 1)
        if cand > end:
            break
        yield (cand, cand.month, True)


_MONTH_CN = {1: "1 月", 2: "2 月", 3: "3 月", 4: "4 月", 5: "5 月", 6: "6 月", 7: "7 月", 8: "8 月", 9: "9 月", 10: "10 月", 11: "11 月", 12: "12 月"}


def render(dark: bool):
    style.apply(dark=dark)
    data = load_data()
    milestones = data["milestones"]
    cats = data["categories"]

    fg = style.INK if dark else "#1a2230"
    color_map = {"TEAL": style.TEAL, "AMBER": style.AMBER, "MUTED": style.MUTED}

    range_start = _d(data["range_start"])
    range_end = _d(data["range_end"])
    x_start = mdates.date2num(range_start)
    x_end = mdates.date2num(range_end)

    fig, ax = plt.subplots(figsize=(11.6, 4.7), dpi=170)

    # Baseline
    ax.plot([x_start, x_end], [0, 0], color=style.MUTED, linewidth=1.6, zorder=2, solid_capstyle="round")
    # small arrowhead marking the direction of time at the right end
    ax.annotate(
        "",
        xy=(x_end, 0),
        xytext=(x_end - (x_end - x_start) * 0.012, 0),
        arrowprops=dict(arrowstyle="-|>", color=style.MUTED, lw=1.6, shrinkA=0, shrinkB=0),
        zorder=2,
    )

    # Weekly tick marks along the baseline (thin ruler rhythm)
    for monday in _mondays_between(range_start, range_end):
        xm = mdates.date2num(monday)
        ax.plot([xm, xm], [-0.05, 0.05], color=style.MUTED, linewidth=0.9, alpha=0.45, zorder=2)

    # Month separators + Chinese month labels
    for label_dt, month, is_boundary in _month_labels_between(range_start, range_end):
        xm = mdates.date2num(label_dt)
        if is_boundary:
            ax.plot([xm, xm], [-1.75, 1.75], color=style.MUTED, linewidth=0.9, linestyle=(0, (2, 3)), alpha=0.3, zorder=1)
        ax.text(xm, -1.9, _MONTH_CN.get(month, f"{month} 月"), fontsize=13, color=style.MUTED, ha="left", va="top", zorder=3)

    # Milestones
    for m in milestones:
        x = mdates.date2num(_d(m["date"]))
        color = color_map.get(cats[m["category"]]["color"], style.MUTED)
        side = 1 if m.get("side", "up") == "up" else -1
        is_abl = bool(m.get("abl", False))
        alpha = 0.55 if is_abl else 0.92
        ls = (0, (3, 2)) if is_abl else "solid"

        ax.plot([x, x], [0, side * STEM_H], color=color, linewidth=1.5, alpha=alpha, linestyle=ls, zorder=3)
        ax.scatter([x], [0], s=95, color=color, alpha=alpha, edgecolor=fg, linewidth=0.9, zorder=5)
        ax.scatter([x], [side * STEM_H], s=32, color=color, alpha=alpha, zorder=5)

        # date tag tucked against the baseline, on the empty side of the stem
        date_label = _d(m["date"]).strftime("%-m/%-d")
        ax.annotate(
            date_label,
            xy=(x, 0),
            xytext=(0, -9 * side),
            textcoords="offset points",
            ha="center",
            va="top" if side > 0 else "bottom",
            fontsize=10,
            color=style.MUTED,
            zorder=6,
        )

        label_txt = m["label"] + ("*" if is_abl else "")
        ax.annotate(
            label_txt,
            xy=(x, side * STEM_H),
            xytext=(0, TITLE_GAP_PT * side),
            textcoords="offset points",
            ha="center",
            va="bottom" if side > 0 else "top",
            fontsize=13,
            fontweight="bold",
            color=fg,
            zorder=6,
        )
        ax.annotate(
            m.get("detail", ""),
            xy=(x, side * STEM_H),
            xytext=(0, DETAIL_GAP_PT * side),
            textcoords="offset points",
            ha="center",
            va="bottom" if side > 0 else "top",
            fontsize=8.1,
            color=style.MUTED,
            linespacing=1.4,
            zorder=6,
        )

    ax.set_xlim(x_start, x_end)
    ax.set_ylim(-2.15, 2.15)
    ax.axis("off")

    fig.text(0.012, 0.975, data.get("title", ""), fontsize=20, fontweight="bold", color=fg, ha="left", va="top")
    if data.get("subtitle"):
        fig.text(0.012, 0.905, data["subtitle"], fontsize=13, color=style.MUTED, ha="left", va="top")

    # Legend: three milestone categories
    legend_handles = [
        plt.Line2D([0], [0], marker="o", linestyle="none", markersize=8, markerfacecolor=color_map[cats[k]["color"]], markeredgecolor=fg, markeredgewidth=0.8, label=cats[k]["label"])
        for k in ("parity", "infra", "support")
        if k in cats
    ]
    ax.legend(
        handles=legend_handles,
        loc="upper right",
        bbox_to_anchor=(1.0, 1.06),
        frameon=False,
        fontsize=12,
        labelcolor=fg,
        handletextpad=0.6,
        ncol=3,
        columnspacing=1.2,
    )

    if data.get("footnote"):
        fig.text(0.01, 0.012, data["footnote"], fontsize=10, color=style.MUTED, ha="left", va="bottom")

    fig.tight_layout(rect=(0, 0.035, 1, 0.86))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    suffix = "dark" if dark else "light"
    out_path = OUT_DIR / f"F17_{suffix}.png"
    fig.savefig(out_path, dpi=200, facecolor=style.NAVY if dark else "#FFFFFF", bbox_inches="tight")
    plt.close(fig)
    return out_path


def main():
    dark_path = render(dark=True)
    light_path = render(dark=False)
    print(f"wrote {dark_path}")
    print(f"wrote {light_path}")


if __name__ == "__main__":
    main()
