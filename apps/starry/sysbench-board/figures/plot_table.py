#!/usr/bin/env python3
"""Dark monospace comparison table: StarryOS before-fix -> now vs Linux.
Board-measured on OrangePi-5-Plus (RK3588). 'before' = occupancy scheduler pre
wake-fix (run #9/#10, multi-thread clustered); 'now' = occupancy fork+wake (run #14)."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BG   = "#12131a"   # near-black
GRID = "#3a3d4a"
FG   = "#e6e6e6"
DIM  = "#9aa0aa"
WIN  = "#4ec27a"   # green — StarryOS >= parity / big gain
GAP  = "#e06c6c"   # red   — gap to Linux
HEAD = "#c8ccd4"

# metric, before, now, linux, higher_better, note
ROWS = [
    ("cpu ev/s, 1 thread",            "368",   "910",   "974",   True,  ""),
    ("cpu ev/s, 2 threads",           "1814",  "1794",  "1954",  True,  ""),
    ("cpu ev/s, 4 threads",           "2706",  "3170",  "3894",  True,  ""),
    ("cpu ev/s, 8 threads",           "1816",  "4686",  "5322",  True,  "bold"),
    ("threads test (events)",         "2861",  "18921", "50595", True,  ""),
    ("mutex (total s, lower=better)", "2.29",  "0.87",  "0.46",  False, ""),
    ("mem first-touch (s, lower)",    "0.33",  "0.030", "0.086", False, "win"),
    ("mem write (MiB/s)",             "13347", "15040", "54984", True,  ""),
    ("per-core A76 (ev/s)",           "—",     "910",   "974",   True,  ""),
    ("per-core A55 (ev/s)",           "—",     "370",   "359",   True,  "win"),
]

def ratio(now, linux, higher):
    try:
        n, l = float(now), float(linux)
    except ValueError:
        return "—", DIM
    if higher:
        r = n / l
        col = WIN if r >= 0.95 else (FG if r >= 0.85 else GAP)
        return f"{r:.2f}×", col
    else:  # lower is better -> Linux/now; >1 means StarryOS faster
        r = l / n
        if r >= 1.0:
            return f"{r:.1f}× faster", WIN
        return f"{1/r:.1f}× slower", GAP

fig, ax = plt.subplots(figsize=(10.6, 5.4))
fig.patch.set_facecolor(BG); ax.set_facecolor(BG); ax.axis("off")

cols = ["metric", "before", "now", "Linux", "now ÷ Linux"]
xs   = [0.015, 0.47, 0.60, 0.73, 0.99]
n = len(ROWS) + 1
def y(i): return 1.0 - (i + 0.5) / n

# header
for x, c in zip(xs, cols):
    ax.text(x, y(0), c, color=HEAD, fontfamily="monospace", fontsize=12,
            fontweight="bold", va="center",
            ha="left" if x < 0.4 else "right")
ax.axhline(1.0 - 1.0/n, color=GRID, lw=1.2, xmin=0.01, xmax=0.99)

for i, (m, b, nw, lx, hb, flag) in enumerate(ROWS, start=1):
    yy = y(i)
    bold = "bold" if flag == "bold" else "normal"
    nowcol = WIN if flag in ("win", "bold") else FG
    if flag == "win":
        nowcol = WIN
    ax.text(xs[0], yy, m, color=FG if flag != "bold" else "#ffffff",
            fontfamily="monospace", fontsize=11, va="center", fontweight=bold)
    ax.text(xs[1], yy, b,  color=DIM, fontfamily="monospace", fontsize=11, va="center", ha="right")
    ax.text(xs[2], yy, nw, color=nowcol, fontfamily="monospace", fontsize=11.5, va="center",
            ha="right", fontweight="bold" if flag in ("bold", "win") else "normal")
    ax.text(xs[3], yy, lx, color=DIM, fontfamily="monospace", fontsize=11, va="center", ha="right")
    rtxt, rcol = ratio(nw, lx, hb)
    ax.text(xs[4], yy, rtxt, color=rcol, fontfamily="monospace", fontsize=10.5, va="center", ha="right")
    if i < len(ROWS):
        ax.axhline(1.0 - (i + 1.0)/n, color=GRID, lw=0.5, alpha=0.5, xmin=0.01, xmax=0.99)

ax.set_title("StarryOS vs Linux on RK3588  —  sysbench + memory  (earlier → now)",
             color=HEAD, fontfamily="monospace", fontsize=12.5, pad=14)
fig.text(0.02, 0.02,
         "before = earlier StarryOS this effort (round-robin / clustered scheduler, pre-THP);  "
         "now = final (occupancy fork+wake, Linux CFS-parity placement, + THP).  "
         "green = StarryOS ≥ parity or large gain.",
         color=DIM, fontfamily="monospace", fontsize=8)
ax.set_xlim(0, 1); ax.set_ylim(0, 1)
fig.savefig("fig6_comparison_table.png", facecolor=BG, dpi=200, bbox_inches="tight")
print("wrote fig6_comparison_table.png")
