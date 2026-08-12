#!/usr/bin/env python3
"""F04_ladder — sysbench cpu 8-thread throughput ladder vs Linux baseline.

Reads figures/data/F04.json (baseline + 3 cumulative optimization rungs),
draws 4 bars with a dashed Linux-baseline reference line and per-bar
%-of-Linux annotations, and renders dark + light PNG variants using the
shared house style (figures/../style/style.py).

Bar color encodes provenance, not just magnitude:
  MUTED = pre-optimization baseline
  TEAL  = shipped in a merged PR (#1656 / #1657)
  AMBER = validated in-session, PR not yet merged (board numbers may still
          shift on the final ablation pass — see each bar's "abl" flag)
"""
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style

FIGURES_DIR = Path(__file__).resolve().parents[2]
DATA_PATH = FIGURES_DIR / "data" / "F04.json"
OUT_DIR = FIGURES_DIR / "out"

SERIES_COLOR = {"muted": style.MUTED, "teal": style.TEAL, "amber": style.AMBER}


def load_data():
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def render(data, dark: bool, out_path: Path):
    style.apply(dark=dark)

    bars = data["bars"]
    linux = data["linux_baseline"]
    n = len(bars)
    x = list(range(n))
    values = [b["value"] for b in bars]
    colors = [SERIES_COLOR[b["series"]] for b in bars]
    any_abl = linux.get("abl", False) or any(b.get("abl", False) for b in bars)

    fig, ax = plt.subplots(figsize=(7.4, 4.4), dpi=180)

    bar_width = 0.52
    rects = ax.bar(
        x,
        values,
        width=bar_width,
        color=colors,
        edgecolor="none",
        zorder=3,
    )

    # Hatch + marker any board-pending ("abl") values so a future JSON-only
    # update (real numbers replacing placeholders) is visually distinct
    # without touching this script.
    for rect, b in zip(rects, bars):
        if b.get("abl", False):
            rect.set_hatch("///")
            rect.set_edgecolor(style.NAVY if dark else "#FFFFFF")
            rect.set_linewidth(0.6)

    ymax = max(values + [linux["value"]]) * 1.22
    ax.set_ylim(0, ymax)
    ax.set_xlim(-0.65, n - 1 + 0.65)

    # Dashed Linux reference line.
    ax.axhline(
        linux["value"],
        linestyle=(0, (5, 3)),
        linewidth=1.3,
        color=style.MUTED,
        alpha=0.9,
        zorder=2,
    )
    linux_note = linux["label_zh"] + f" {linux['value']:,}"
    if linux.get("abl", False):
        linux_note += " †"
    ax.text(
        n - 1 + 0.65,
        linux["value"] + ymax * 0.018,
        linux_note,
        ha="right",
        va="bottom",
        fontsize=9.5,
        color=style.MUTED,
    )

    # Per-bar value + %-of-Linux annotation. When a bar sits close under the
    # Linux reference line there isn't room to stack a 2-line label above it
    # without touching the dashed line, so the label drops inside the bar
    # instead (dark-on-fill reads on both amber and teal).
    clearance_needed = ymax * 0.16
    for xi, b in zip(x, bars):
        pct = 100.0 * b["value"] / linux["value"]
        label = f"{b['value']:,}"
        if b.get("abl", False):
            label += " †"
        label += f"\n{pct:.0f}% Linux"

        crowds_line = 0 <= (linux["value"] - b["value"]) < clearance_needed
        if crowds_line:
            ax.text(
                xi,
                b["value"] - ymax * 0.02,
                label,
                ha="center",
                va="top",
                fontsize=9.5,
                linespacing=1.35,
                color=style.NAVY,
                fontweight="bold",
            )
        else:
            ax.text(
                xi,
                b["value"] + ymax * 0.018,
                label,
                ha="center",
                va="bottom",
                fontsize=9.5,
                linespacing=1.35,
            )

    # X tick labels: primary Chinese label + provenance sub-label.
    tick_labels = []
    for b in bars:
        lbl = b["label_zh"]
        if b.get("sublabel_zh"):
            lbl += f"\n{b['sublabel_zh']}"
        tick_labels.append(lbl)
    ax.set_xticks(x)
    ax.set_xticklabels(tick_labels, fontsize=10)
    ax.tick_params(axis="x", length=0)

    ax.set_ylabel(data.get("unit_zh", "事件数/秒 (ev/s)"), fontsize=10)
    ax.set_title(
        data.get("title_zh", "F04"),
        fontsize=13,
        fontweight="bold",
        pad=14,
    )
    if data.get("subtitle_zh"):
        ax.text(
            0.0,
            1.02,
            data["subtitle_zh"],
            transform=ax.transAxes,
            fontsize=9.5,
            color=style.MUTED,
            ha="left",
            va="bottom",
        )

    ax.grid(axis="y", zorder=0)
    ax.grid(axis="x", visible=False)
    ax.set_axisbelow(True)

    # Legend: color encodes provenance, not the metric itself.
    legend_zh = data.get("legend_zh", {})
    legend_handles = [
        Patch(facecolor=SERIES_COLOR["muted"], label=legend_zh.get("muted", "基线")),
        Patch(facecolor=SERIES_COLOR["teal"], label=legend_zh.get("teal", "已合并 PR")),
        Patch(
            facecolor=SERIES_COLOR["amber"],
            label=legend_zh.get("amber", "会话内验证"),
        ),
        Line2D(
            [0],
            [0],
            color=style.MUTED,
            linestyle=(0, (5, 3)),
            linewidth=1.3,
            label=linux["label_zh"],
        ),
    ]
    ax.legend(
        handles=legend_handles,
        loc="upper left",
        frameon=False,
        fontsize=8.8,
        handlelength=1.6,
        borderaxespad=0.0,
    )

    footnote_lines = []
    if any_abl:
        footnote_lines.append(
            "† 板上复测前占位值，最终以 figures/data/F04.json 板上复测结果为准"
        )
    if data.get("footnote_zh"):
        footnote_lines.append(data["footnote_zh"])
    if footnote_lines:
        ax.text(
            0.0,
            -0.16,
            "\n".join(footnote_lines),
            transform=ax.transAxes,
            fontsize=8,
            color=style.MUTED,
            ha="left",
            va="top",
        )

    fig.tight_layout()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def main():
    data = load_data()
    render(data, dark=True, out_path=OUT_DIR / "F04_dark.png")
    render(data, dark=False, out_path=OUT_DIR / "F04_light.png")
    print(f"wrote {OUT_DIR / 'F04_dark.png'}")
    print(f"wrote {OUT_DIR / 'F04_light.png'}")


if __name__ == "__main__":
    main()
