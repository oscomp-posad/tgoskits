#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""F13_modelopt — QEMU RKNPU 模拟器实测：模型结构协同优化前后对比.

Three small-multiple before/after panels (MAC 计算量 / LUT 任务数 /
rknn_run 耗时) share one row and one visual language — MUTED = 原始基线,
TEAL = 协同优化后 — plus a narrower, dashed-outline fourth panel that is
explicitly a *note* rather than a coequal cost metric: mAP50 精度基本持平
(92.9% -> 91.1%), drawn in AMBER to flag the (small) accuracy trade-off.

All four values pairs are QEMU RKNPU functional-emulator measurements
(processmission/qemu #19 / #26) — i.e.
"模拟器实测", not a board run — so every leaf in figures/data/F13.json
carries "abl": false today. The per-value "abl" flag is still read (and
would render as a hatch + dagger) so a future board-corroboration pass can
flip individual values without touching this script's layout.

Data: figures/data/F13.json
Usage: python3 figures/src/charts/F13_modelopt.py
Writes figures/out/F13_dark.png and figures/out/F13_light.png.
"""
import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

HERE = Path(__file__).resolve()
FIGURES_DIR = HERE.parents[2]
DATA_PATH = FIGURES_DIR / "data" / "F13.json"
OUT_DIR = FIGURES_DIR / "out"

BAR_WIDTH = 0.56
ABL_HATCH = "///"


def load_data() -> dict:
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def draw_pair(ax, before: dict, after: dict, fmt: str, before_color: str, after_color: str, fg: str, unit_suffix: str = ""):
    """Draw one 原始/优化后 bar pair on `ax`; return (ymax, has_pending)."""
    values = [before["value"], after["value"]]
    colors = [before_color, after_color]
    abls = [before.get("abl", False), after.get("abl", False)]
    ymax = max(values) * 1.42
    has_pending = False

    for xi, val, col, is_abl in zip((0, 1), values, colors, abls):
        ax.bar(
            xi,
            val,
            width=BAR_WIDTH,
            color=col,
            alpha=0.55 if is_abl else 0.92,
            edgecolor=col,
            linewidth=1.1,
            hatch=ABL_HATCH if is_abl else None,
            zorder=3,
        )
        label = fmt.format(val) + unit_suffix
        if is_abl:
            label += "†"
            has_pending = True
        ax.text(
            xi,
            val + ymax * 0.028,
            label,
            ha="center",
            va="bottom",
            fontsize=13,
            color=fg,
            fontweight="bold",
            zorder=4,
        )

    ax.set_xticks([0, 1])
    ax.set_xticklabels(["原始", "优化后"], fontsize=12)
    ax.set_ylim(0, ymax)
    ax.set_xlim(-0.68, 1.68)
    ax.tick_params(axis="both", length=0)
    ax.set_yticklabels([])
    ax.grid(axis="y", alpha=0.16, zorder=0)
    ax.grid(axis="x", visible=False)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_visible(False)
    return ymax, has_pending


def render(dark: bool, data: dict, out_path: Path) -> None:
    style.apply(dark=dark)

    fg = style.INK if dark else "#1a2230"
    muted = style.MUTED

    metrics = data["metrics"]
    map50 = data["map50_note"]

    fig = plt.figure(figsize=(11.8, 4.7), dpi=200)
    gs = fig.add_gridspec(1, 4, width_ratios=[1, 1, 1, 0.78], wspace=0.55, left=0.045, right=0.985, top=0.68, bottom=0.20)
    axes = [fig.add_subplot(gs[0, i]) for i in range(4)]

    has_pending = False

    # Three cost-metric panels: MUTED (原始) -> TEAL (优化后).
    for ax, m in zip(axes[:3], metrics):
        ymax, pending = draw_pair(
            ax,
            m["before"],
            m["after"],
            m["fmt"],
            before_color=muted,
            after_color=style.TEAL,
            fg=fg,
        )
        has_pending = has_pending or pending
        ax.set_title(f"{m['label_zh']}\n({m['unit_zh']})", fontsize=14, color=fg, pad=10)

        b, a = m["before"]["value"], m["after"]["value"]
        reduction = (b - a) / b * 100.0
        ax.text(
            0.5,
            0.985,
            f"↓{reduction:.0f}%",
            transform=ax.transAxes,
            ha="center",
            va="top",
            fontsize=14,
            fontweight="bold",
            color=style.TEAL,
            zorder=5,
        )

    # Fourth panel: the mAP50 note, MUTED (原始) -> AMBER (优化后, 精度权衡),
    # visually set apart with a dashed outline so it reads as an annotation
    # rather than a fourth coequal cost bar.
    ax4 = axes[3]
    _, pending4 = draw_pair(
        ax4,
        map50["before"],
        map50["after"],
        map50["fmt"],
        before_color=muted,
        after_color=style.AMBER,
        fg=fg,
        unit_suffix=map50["unit_zh"],
    )
    has_pending = has_pending or pending4
    ax4.set_title(f"{map50['label_zh']}\n（附注 · {map50['unit_zh']}）", fontsize=13, color=fg, pad=10)
    for side in ("top", "right", "left", "bottom"):
        ax4.spines[side].set_visible(True)
        ax4.spines[side].set_linestyle((0, (4, 3)))
        ax4.spines[side].set_color(style.AMBER)
        ax4.spines[side].set_linewidth(1.0)
        ax4.spines[side].set_alpha(0.75)

    delta = map50["after"]["value"] - map50["before"]["value"]
    ax4.text(
        0.5,
        0.985,
        f"{delta:+.1f}pp",
        transform=ax4.transAxes,
        ha="center",
        va="top",
        fontsize=14,
        fontweight="bold",
        color=style.AMBER,
        zorder=5,
    )

    # Title / subtitle.
    fig.suptitle(data["title_zh"], x=0.045, y=0.975, ha="left", fontsize=21, fontweight="bold", color=fg)
    fig.text(0.045, 0.865, data["subtitle_zh"], fontsize=11, color=muted, ha="left", va="top")

    # Shared legend: MUTED=原始基线, TEAL=优化后(成本类指标), AMBER=优化后(精度附注).
    legend_handles = [
        plt.Rectangle((0, 0), 1, 1, facecolor=muted, edgecolor="none", label=data["series_labels"]["before"]),
        plt.Rectangle((0, 0), 1, 1, facecolor=style.TEAL, edgecolor="none", label="优化后 · 成本类指标下降"),
        plt.Rectangle((0, 0), 1, 1, facecolor=style.AMBER, edgecolor="none", label="优化后 · 精度附注（小幅权衡）"),
    ]
    if has_pending:
        legend_handles.append(
            plt.Rectangle(
                (0, 0), 1, 1, facecolor=muted, alpha=0.55, hatch=ABL_HATCH,
                edgecolor=muted, linewidth=0.8, label="† 板级消融待验证",
            )
        )
    legend = fig.legend(
        handles=legend_handles,
        loc="upper right",
        bbox_to_anchor=(0.985, 0.99),
        frameon=False,
        fontsize=12,
        handlelength=1.3,
        handleheight=1.1,
        ncol=1,
    )
    for text in legend.get_texts():
        text.set_color(fg)

    # Lever + provenance footnotes.
    fig.text(0.045, 0.10, data["lever_note_zh"], fontsize=12, color=fg, ha="left", va="top")
    fig.text(0.045, 0.045, data["footnote_zh"], fontsize=8.5, color=muted, ha="left", va="top")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=200)
    plt.close(fig)
    print(f"wrote {out_path}")


def main() -> None:
    data = load_data()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    render(dark=True, data=data, out_path=OUT_DIR / "F13_dark.png")
    render(dark=False, data=data, out_path=OUT_DIR / "F13_light.png")


if __name__ == "__main__":
    main()
