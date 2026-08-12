"""F07_getpid — getpid syscall-entry micro-ladder.

Descending-bar ladder: baseline -> ptrace fast-path -> skip
poll_process_timer -> lock-free timer_state, with a dashed Linux
reference line. Data lives in figures/data/F07.json; intermediate
steps flagged "abl": true are provisional (board ablation pending)
and are drawn with a dashed outline + lower alpha + a trailing "*".
"""

import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt

sys.path.insert(0, "/Users/jsph273/Desktop/Code/optimization-efforts-final/style")
import style  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]  # .../figures
DATA_PATH = ROOT / "data" / "F07.json"
OUT_DIR = ROOT / "out"

BAR_WIDTH = 0.56


def load_data():
    with open(DATA_PATH, encoding="utf-8") as f:
        return json.load(f)


def render(dark: bool):
    style.apply(dark=dark)
    data = load_data()
    stages = data["stages"]
    linux_ref = data["linux_ref"]

    fg = style.INK if dark else "#1a2230"
    role_color = {
        "baseline": style.MUTED,
        "step": style.AMBER,
        "final": style.TEAL,
    }

    labels = [s["label"] for s in stages]
    values = [s["value"] for s in stages]
    x = list(range(len(stages)))

    fig, ax = plt.subplots(figsize=(5.6, 3.5), dpi=180)

    for xi, s in zip(x, stages):
        color = role_color[s["role"]]
        is_abl = bool(s.get("abl", False))
        ax.bar(
            xi,
            s["value"],
            width=BAR_WIDTH,
            color=color,
            alpha=0.55 if is_abl else 0.95,
            edgecolor=color,
            linewidth=1.3,
            linestyle=(0, (3, 2)) if is_abl else "solid",
            zorder=3,
        )
        label_txt = f"{s['value']:.0f}{'*' if is_abl else ''}"
        ax.annotate(
            label_txt,
            xy=(xi, s["value"]),
            xytext=(0, 5),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9.5,
            color=fg,
            zorder=4,
        )

    # Linux reference line
    ax.axhline(
        linux_ref["value"],
        color=style.MUTED,
        linewidth=1.1,
        linestyle=(0, (5, 3)),
        zorder=2,
    )
    ax.annotate(
        f"{linux_ref['label']} {linux_ref['value']:.0f} ns",
        xy=(len(stages) - 1 + BAR_WIDTH / 2, linux_ref["value"]),
        xytext=(-2, 5),
        textcoords="offset points",
        ha="right",
        va="bottom",
        fontsize=8.5,
        color=style.MUTED,
        zorder=4,
    )

    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=9)
    ax.set_ylabel(data.get("y_label", "延迟 (ns)"), fontsize=10)
    ax.set_ylim(0, max(values) * 1.18)
    ax.set_xlim(-0.65, len(stages) - 1 + 0.65)

    ax.grid(axis="y", zorder=0)
    ax.grid(axis="x", visible=False)
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="both", length=0)

    if data.get("title"):
        ax.set_title(data["title"], fontsize=12, pad=14, color=fg)

    # legend explaining the three bar roles
    legend_handles = [
        plt.Rectangle((0, 0), 1, 1, facecolor=style.MUTED, alpha=0.95, edgecolor=style.MUTED, label="基线（已定值）"),
        plt.Rectangle(
            (0, 0),
            1,
            1,
            facecolor=style.AMBER,
            alpha=0.55,
            edgecolor=style.AMBER,
            linestyle=(0, (3, 2)),
            linewidth=1.3,
            label="中间步骤（* 待整机验证）",
        ),
        plt.Rectangle((0, 0), 1, 1, facecolor=style.TEAL, alpha=0.95, edgecolor=style.TEAL, label="最终值（已定值）"),
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
            fontsize=6.8,
            color=style.MUTED,
            ha="left",
            va="bottom",
        )

    fig.tight_layout(rect=(0, 0.035, 1, 1))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    suffix = "dark" if dark else "light"
    out_path = OUT_DIR / f"F07_{suffix}.png"
    fig.savefig(out_path, dpi=180)
    plt.close(fig)
    return out_path


def main():
    dark_path = render(dark=True)
    light_path = render(dark=False)
    print(f"wrote {dark_path}")
    print(f"wrote {light_path}")


if __name__ == "__main__":
    main()
