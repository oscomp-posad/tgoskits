#!/usr/bin/env python3
"""F06_waterfall — scheduler-stack gap-decomposition waterfall.

Rising floating bars: StarryOS's flat A55 baseline (159 ev/s) climbs through
three multiplicative levers — DVFS (x2.25), A55->A76 placement (x2.72), and
load balancing 1->4 cores (x3.98) — to a solid "optimized" subtotal (~3873),
then a final floating "残差" (residual) bar bridges the remaining gap up to
the dashed Linux reference line (5322 ev/s). Data lives in figures/data/F06.json;
stages flagged "abl": true are provisional (board ablation pending) and are
drawn with a dashed outline + lower alpha + a trailing "*".

Usage: python3 figures/src/charts/F06_waterfall.py
Writes figures/out/F06_dark.png and figures/out/F06_light.png.
"""

import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt

FIGURES_DIR = Path(__file__).resolve().parents[2]  # .../figures
sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

DATA_PATH = FIGURES_DIR / "data" / "F06.json"
OUT_DIR = FIGURES_DIR / "out"

BAR_WIDTH = 0.62
GAP_HATCH = "///"


def load_data():
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def render(dark: bool):
    style.apply(dark=dark)
    data = load_data()
    stages = data["stages"]
    linux_ref = data["linux_ref"]

    fg = style.INK if dark else "#1a2230"
    surface = style.NAVY if dark else "#FFFFFF"

    role_color = {
        "baseline": style.MUTED,
        "step": style.TEAL,
        "final": style.AMBER,
        "gap": style.MUTED,
    }

    x = list(range(len(stages)))
    fig, ax = plt.subplots(figsize=(9.2, 5.4), dpi=200)

    tops = []
    for xi, s in zip(x, stages):
        role = s["role"]
        color = role_color[role]
        is_abl = bool(s.get("abl", False))
        is_gap = role == "gap"
        bottom = s["bottom"]
        top = s["top"]
        height = top - bottom

        ax.bar(
            xi,
            height,
            bottom=bottom,
            width=BAR_WIDTH,
            color=color,
            alpha=0.42 if is_gap else (0.55 if is_abl else 0.92),
            edgecolor=color,
            linewidth=1.3,
            linestyle=(0, (3, 2)) if (is_abl and not is_gap) else "solid",
            hatch=GAP_HATCH if is_gap else None,
            zorder=3,
        )
        tops.append(top)

        # connector: thin dotted line from this bar's top to the next bar's
        # start, classic waterfall "bridge".
        if xi < len(stages) - 1:
            ax.plot(
                [xi + BAR_WIDTH / 2, xi + 1 - BAR_WIDTH / 2],
                [top, top],
                color=style.MUTED,
                linewidth=0.9,
                linestyle=(0, (1, 1.6)),
                alpha=0.55,
                zorder=2,
            )

        # value label above the bar top — skipped for the gap bar, whose top
        # is by construction the same value already labeled on the Linux
        # reference line just above it (avoids an overlapping duplicate).
        if not is_gap:
            star = "*" if is_abl else ""
            value_txt = f"{top:,.0f}{star}"
            ax.annotate(
                value_txt,
                xy=(xi, top),
                xytext=(0, 6),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=9.5,
                fontweight="bold" if role == "final" else "normal",
                color=fg,
                zorder=4,
            )

        # multiplier / gap tag inside the floating segment.
        if s.get("tag"):
            mid = bottom + height / 2
            tag_txt = f"+{height:,.0f}\n{s['tag']}" if is_gap else s["tag"]
            ax.annotate(
                tag_txt,
                xy=(xi, mid),
                ha="center",
                va="center",
                fontsize=8.5,
                color=fg,
                zorder=4,
            )

    # Linux reference line.
    ax.axhline(
        linux_ref["value"],
        color=style.MUTED,
        linewidth=1.1,
        linestyle=(0, (5, 3)),
        zorder=2,
    )
    ax.annotate(
        f"{linux_ref['label']} {linux_ref['value']:,.0f}",
        xy=(len(stages) - 1 + BAR_WIDTH / 2, linux_ref["value"]),
        xytext=(-2, 5),
        textcoords="offset points",
        ha="right",
        va="bottom",
        fontsize=8.8,
        color=style.MUTED,
        zorder=4,
    )

    labels = [s["label"] for s in stages]
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=9.3)
    ax.set_ylabel(data.get("y_label", "吞吐 (events/s)"), fontsize=10)
    ax.set_ylim(0, max(max(tops), linux_ref["value"]) * 1.16)
    ax.set_xlim(-0.65, len(stages) - 1 + 0.65)

    ax.grid(axis="y", zorder=0)
    ax.grid(axis="x", visible=False)
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="both", length=0)

    if data.get("title"):
        ax.set_title(data["title"], fontsize=13, fontweight="bold", pad=30, color=fg, loc="left")
    if data.get("subtitle"):
        ax.text(
            0.0,
            1.035,
            data["subtitle"],
            transform=ax.transAxes,
            fontsize=8.6,
            color=style.MUTED,
            ha="left",
            va="bottom",
        )

    # legend explaining bar roles.
    legend_handles = [
        plt.Rectangle((0, 0), 1, 1, facecolor=style.MUTED, alpha=0.92, edgecolor=style.MUTED, label="基线"),
        plt.Rectangle(
            (0, 0),
            1,
            1,
            facecolor=style.TEAL,
            alpha=0.55,
            edgecolor=style.TEAL,
            linestyle=(0, (3, 2)),
            linewidth=1.3,
            label="优化步骤（* 待整机验证）",
        ),
        plt.Rectangle((0, 0), 1, 1, facecolor=style.AMBER, alpha=0.55, edgecolor=style.AMBER, linestyle=(0, (3, 2)), linewidth=1.3, label="StarryOS 优化后合计（*）"),
        plt.Rectangle((0, 0), 1, 1, facecolor=surface, edgecolor=style.MUTED, hatch=GAP_HATCH, alpha=0.9, label="残差（待 board 消融）"),
    ]
    ax.legend(
        handles=legend_handles,
        loc="upper left",
        frameon=False,
        fontsize=7.6,
        labelcolor=fg,
        handlelength=1.3,
        handleheight=1.1,
    )

    if data.get("footnote"):
        fig.text(
            0.02,
            0.01,
            data["footnote"],
            fontsize=6.8,
            color=style.MUTED,
            ha="left",
            va="bottom",
        )

    fig.tight_layout(rect=(0, 0.035, 1, 1))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    suffix = "dark" if dark else "light"
    out_path = OUT_DIR / f"F06_{suffix}.png"
    fig.savefig(out_path, dpi=200, facecolor=surface, bbox_inches="tight")
    plt.close(fig)
    return out_path


def main():
    dark_path = render(dark=True)
    light_path = render(dark=False)
    print(f"wrote {dark_path}")
    print(f"wrote {light_path}")


if __name__ == "__main__":
    main()
