"""F08_thp — THP first-touch mapping-establish time, three-bar comparison.

Three bars: 未开启 THP (board-pending placeholder), 开启 THP, Linux. Lower is
better (seconds). A bracket annotation calls out the ratio between 开启 THP and
Linux (2.87x in the seed data). Data lives in figures/data/F08.json;
board-pending values carry "abl": true and are drawn with a hatch + dashed
outline + lower alpha + a trailing "*".

Usage: python3 figures/src/charts/F08_thp.py
Writes figures/out/F08_dark.png and figures/out/F08_light.png.
"""

import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]  # .../figures
DATA_PATH = ROOT / "data" / "F08.json"
OUT_DIR = ROOT / "out"

BAR_WIDTH = 0.5
ABL_HATCH = "///"


def load_data():
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def render(dark: bool):
    style.apply(dark=dark)
    data = load_data()
    bars = data["bars"]
    ann = data.get("annotation", {})

    fg = style.INK if dark else "#1a2230"
    surface = style.NAVY if dark else "#FFFFFF"

    role_color = {
        "baseline": style.MUTED,
        "final": style.TEAL,
        "reference": style.AMBER,
    }

    labels = [b["label"] for b in bars]
    values = [b["value"] for b in bars]
    x = list(range(len(bars)))

    fig, ax = plt.subplots(figsize=(5.4, 3.6), dpi=180)

    for xi, b in zip(x, bars):
        color = role_color[b["role"]]
        is_abl = bool(b.get("abl", False))
        ax.bar(
            xi,
            b["value"],
            width=BAR_WIDTH,
            color=color,
            alpha=0.55 if is_abl else 0.92,
            edgecolor=color,
            linewidth=1.3,
            linestyle=(0, (3, 2)) if is_abl else "solid",
            hatch=ABL_HATCH if is_abl else None,
            zorder=3,
        )
        label_txt = f"{b['value']:.3f} s{'*' if is_abl else ''}"
        ax.annotate(
            label_txt,
            xy=(xi, b["value"]),
            xytext=(0, 6),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9.5,
            color=fg,
            zorder=4,
        )

    # Bracket annotation between the two non-placeholder bars. Sized as a
    # fraction of the *full* y-range (not the two bars' own small scale) so
    # the riser clears each bar's value label with margin regardless of how
    # the placeholder bar's magnitude shifts once real numbers land — the
    # placeholder is, by construction, always far larger than these two.
    y_top = max(values) * 1.18
    by_key = {b["key"]: (xi, b["value"]) for xi, b in zip(x, bars)}
    if ann.get("from") in by_key and ann.get("to") in by_key:
        x1, v1 = by_key[ann["from"]]
        x2, v2 = by_key[ann["to"]]
        if x1 > x2:
            x1, v1, x2, v2 = x2, v2, x1, v1
        label_clear = y_top * 0.10  # clears the "N.NNN s" label above each bar
        bracket_y = y_top * 0.30
        ax.plot(
            [x1, x1, x2, x2],
            [v1 + label_clear, bracket_y, bracket_y, v2 + label_clear],
            color=fg,
            linewidth=1.0,
            zorder=5,
        )
        ax.annotate(
            ann.get("ratio_label", ""),
            xy=((x1 + x2) / 2, bracket_y),
            xytext=(0, 5),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=12,
            fontweight="bold",
            color=style.TEAL,
            zorder=5,
        )
        if ann.get("text"):
            ax.annotate(
                ann["text"],
                xy=((x1 + x2) / 2, bracket_y),
                xytext=(0, 24),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8,
                color=style.MUTED,
                zorder=5,
            )

    ax.set_xticks(x)
    ax.set_xticklabels(
        [lab + ("*" if b.get("abl") else "") for lab, b in zip(labels, bars)],
        fontsize=9.5,
    )
    ax.set_ylabel(data.get("y_label", "耗时（秒 / s）"), fontsize=10)
    ax.set_ylim(0, max(values) * 1.18)
    ax.set_xlim(-0.75, len(bars) - 1 + 0.75)

    ax.grid(axis="y", zorder=0)
    ax.grid(axis="x", visible=False)
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="both", length=0)

    if data.get("title"):
        ax.set_title(data["title"], fontsize=12.5, pad=14, color=fg)

    # legend explaining the three bar roles
    legend_handles = [
        plt.Rectangle(
            (0, 0), 1, 1, facecolor=surface, edgecolor=style.MUTED, alpha=0.9,
            hatch=ABL_HATCH, linestyle=(0, (3, 2)), linewidth=1.3,
            label="未开启 THP（占位，* 待板级验证）",
        ),
        plt.Rectangle((0, 0), 1, 1, facecolor=style.TEAL, edgecolor=style.TEAL, alpha=0.92, label="开启 THP（已定值）"),
        plt.Rectangle((0, 0), 1, 1, facecolor=style.AMBER, edgecolor=style.AMBER, alpha=0.92, label="Linux 基线（已定值）"),
    ]
    ax.legend(
        handles=legend_handles,
        loc="upper right",
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
            fontsize=6.6,
            color=style.MUTED,
            ha="left",
            va="bottom",
        )

    fig.tight_layout(rect=(0, 0.06, 1, 1))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    suffix = "dark" if dark else "light"
    out_path = OUT_DIR / f"F08_{suffix}.png"
    fig.savefig(out_path, dpi=180, facecolor=surface)
    plt.close(fig)
    return out_path


def main():
    dark_path = render(dark=True)
    light_path = render(dark=False)
    print(f"wrote {dark_path}")
    print(f"wrote {light_path}")


if __name__ == "__main__":
    main()
