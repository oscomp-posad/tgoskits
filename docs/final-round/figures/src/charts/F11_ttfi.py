#!/usr/bin/env python3
"""F11_ttfi — TTFI (time-to-first-inference) boot-chain decomposition.

Segmented horizontal bars for StarryOS BEFORE (19.22 s) vs AFTER (9.83 s),
split into boot-chain stages (PCIe probe, block-IRQ fallback, model_init,
first-inference), with a dashed reference line at the measured Linux TTFI
(11.28 s).

Data:   figures/data/F11.json
Output: figures/out/F11_dark.png, figures/out/F11_light.png
"""
import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import Patch

FIGURES_DIR = Path(__file__).resolve().parents[2]
DATA_PATH = FIGURES_DIR / "data" / "F11.json"
OUT_DIR = FIGURES_DIR / "out"

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

BAR_H = 0.5
Y_POS = {"before": 1.0, "after": 0.0}


def stage_style(stage_key: str):
    """Map each of the 4 boot-chain stages onto TEAL / AMBER / MUTED only,
    using alpha to separate the two MUTED (fixed-overhead) sub-stages."""
    return {
        "pcie_probe": {"color": style.MUTED, "alpha": 1.0},
        "block_irq_fallback": {"color": style.MUTED, "alpha": 0.5},
        "model_init": {"color": style.TEAL, "alpha": 1.0},
        "first_inference": {"color": style.AMBER, "alpha": 1.0},
    }[stage_key]


def load_data():
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def render(dark: bool, out_path: Path):
    style.apply(dark=dark)
    data = load_data()

    fg = style.INK if dark else "#1a2230"
    grid_line = style.MUTED

    fig, ax = plt.subplots(figsize=(8.6, 4.3), dpi=150)
    fig.subplots_adjust(left=0.1, right=0.97, top=0.86, bottom=0.34)

    edge_color = "#0A1622" if dark else "#FFFFFF"
    label_color = style.NAVY

    x_max = 0.0
    for series in data["series"]:
        y = Y_POS[series["id"]]
        cursor = 0.0
        for key in data["stage_order"]:
            stage = series["stages"][key]
            val = stage["value"]
            sty = stage_style(key)
            ax.barh(
                y,
                val,
                left=cursor,
                height=BAR_H,
                color=sty["color"],
                alpha=sty["alpha"],
                edgecolor=edge_color,
                linewidth=0.8,
            )
            if val >= 1.2:
                ax.text(
                    cursor + val / 2.0,
                    y,
                    f"{val:.2f}",
                    ha="center",
                    va="center",
                    fontsize=8,
                    color=label_color,
                )
            cursor += val

        ax.text(
            cursor + 0.22,
            y,
            f"{series['total']:.2f} s",
            ha="left",
            va="center",
            fontsize=10.5,
            fontweight="bold",
            color=fg,
        )
        x_max = max(x_max, cursor)

    linux_val = data["linux"]["value"]
    x_max = max(x_max, linux_val) * 1.16

    ax.set_ylim(-0.55, 2.15)
    ax.axvline(
        linux_val, ymin=0.0, ymax=0.86, color=fg, linestyle=(0, (4, 3)), linewidth=1.3, zorder=5
    )
    ax.text(
        linux_val + x_max * 0.012,
        2.0,
        f"{data['linux']['label']} {linux_val:.2f} s",
        ha="left",
        va="top",
        fontsize=9,
        color=fg,
    )

    ax.set_yticks([Y_POS["before"], Y_POS["after"]])
    ax.set_yticklabels(
        [
            next(s["label"] for s in data["series"] if s["id"] == "before"),
            next(s["label"] for s in data["series"] if s["id"] == "after"),
        ],
        fontsize=11,
    )
    ax.set_xlim(0, x_max)
    ax.set_xlabel("时间 (秒)", fontsize=10, labelpad=6)
    ax.set_title(data["title"], fontsize=13, fontweight="bold", pad=14, color=fg)

    ax.grid(axis="x", color=grid_line, alpha=0.18)
    ax.grid(axis="y", visible=False)
    ax.tick_params(axis="y", length=0)

    legend_handles = [
        Patch(
            facecolor=stage_style(key)["color"],
            alpha=stage_style(key)["alpha"],
            edgecolor="none",
            label=data["stage_labels"][key],
        )
        for key in data["stage_order"]
    ]
    fig.legend(
        handles=legend_handles,
        loc="lower center",
        bbox_to_anchor=(0.5, 0.11),
        ncol=4,
        frameon=False,
        fontsize=8.5,
        handlelength=1.6,
        handletextpad=0.6,
        columnspacing=1.6,
    )

    fig.text(
        0.01,
        0.015,
        "各阶段切分为估算值，待整板复测确认更新（改造前/改造后/Linux 总耗时已由串口时间戳板测确认）",
        fontsize=7,
        color=grid_line,
        ha="left",
        va="bottom",
    )

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def main():
    render(dark=True, out_path=OUT_DIR / "F11_dark.png")
    render(dark=False, out_path=OUT_DIR / "F11_light.png")
    print(f"wrote {OUT_DIR / 'F11_dark.png'}")
    print(f"wrote {OUT_DIR / 'F11_light.png'}")


if __name__ == "__main__":
    main()
