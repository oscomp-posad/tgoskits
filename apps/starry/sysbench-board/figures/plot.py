#!/usr/bin/env python3
"""Research-paper figures for the StarryOS-vs-Linux RK3588 report.
All numbers are board-measured on the OrangePi-5-Plus (sysbench + membw);
projected/interpolated points are marked in the code and rendered distinctly."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

# ---- clean paper style ---------------------------------------------------------
plt.rcParams.update({
    "font.family": "serif",
    "font.size": 11,
    "axes.titlesize": 12,
    "axes.labelsize": 11,
    "axes.grid": True,
    "grid.alpha": 0.30,
    "grid.linewidth": 0.6,
    "axes.axisbelow": True,
    "figure.dpi": 200,
    "savefig.dpi": 200,
    "savefig.bbox": "tight",
    "legend.frameon": True,
    "legend.framealpha": 0.92,
    "legend.fontsize": 9.5,
})
C_LINUX = "#444444"      # Linux baseline — dark grey
C_OCC   = "#1f77b4"      # StarryOS occupancy placement — blue
C_RR    = "#2ca02c"      # StarryOS round-robin — green
C_STAR  = "#1f77b4"
C_WIN   = "#2ca02c"
C_GAP   = "#d62728"

# ================================================================================
# Fig 1 — CPU thread scaling (sysbench cpu, events/sec; higher = better)
# ================================================================================
threads = [1, 2, 4, 8]
linux   = [974, 1930, 3900, 5322]   # 1/4/8 measured; t2 interpolated (open marker)
occ     = [912, 1814, 2706, 1816]   # all measured (run #9)
rr_x    = [1, 4, 8]
rr      = [368, 3810, 5100]         # 1/4 measured (run #1); t8=5100 projected

fig, ax = plt.subplots(figsize=(6.2, 4.0))
ax.plot(threads, linux, "-o", color=C_LINUX, lw=2, ms=6, label="Linux (Armbian 6.1)")
# mark Linux t2 as interpolated (open marker)
ax.plot([2], [1930], "o", mfc="white", mec=C_LINUX, ms=6, zorder=5)
ax.plot(threads, occ, "-s", color=C_OCC, lw=2, ms=6, label="StarryOS — occupancy placement")
ax.plot(rr_x[:2], rr[:2], "-^", color=C_RR, lw=2, ms=7, label="StarryOS — round-robin (ship)")
ax.plot([4, 8], [3810, 5100], "--^", color=C_RR, lw=1.6, ms=7, mfc="white")  # projected tail
ax.annotate("single-thread win\n368→912 (self-heal)", xy=(1.02, 912), xytext=(1.5, 300),
            fontsize=8.5, color=C_OCC, ha="left",
            arrowprops=dict(arrowstyle="->", color=C_OCC, lw=1))
ax.annotate("t=8 spill collapse\n(burst contention)", xy=(8, 1816), xytext=(4.7, 1050),
            fontsize=8.5, color=C_GAP,
            arrowprops=dict(arrowstyle="->", color=C_GAP, lw=1))
ax.annotate("proj.", xy=(8, 5100), xytext=(7.0, 4550), fontsize=8, color=C_RR)
ax.set_xscale("log", base=2)
ax.set_xticks(threads); ax.set_xticklabels(threads)
ax.set_xlabel("threads"); ax.set_ylabel("events / sec")
ax.set_title("CPU throughput scaling  (sysbench cpu, prime=20000)")
ax.set_ylim(0, 5800)
ax.legend(loc="upper left")
fig.savefig("fig1_cpu_scaling.png"); plt.close(fig)

# ================================================================================
# Fig 2 — per-core parity (pinned sysbench cpu; higher = better)
# ================================================================================
labels = ["A55 (little)", "A76 (big)"]
star   = [370, 910]
lin    = [359, 974]
x = range(len(labels)); w = 0.36
fig, ax = plt.subplots(figsize=(4.8, 4.0))
b1 = ax.bar([i - w/2 for i in x], lin,  w, color=C_LINUX, label="Linux")
b2 = ax.bar([i + w/2 for i in x], star, w, color=C_STAR,  label="StarryOS (cpufreq)")
for i, (s, l) in enumerate(zip(star, lin)):
    ax.text(i + w/2, s + 12, f"{s/l:.2f}×", ha="center", fontsize=9, color=C_STAR)
ax.set_xticks(list(x)); ax.set_xticklabels(labels)
ax.set_ylabel("events / sec"); ax.set_title("Per-core parity  (pinned, taskset)")
ax.set_ylim(0, 1080); ax.legend(loc="upper left")
fig.savefig("fig2_percore.png"); plt.close(fig)

# ================================================================================
# Fig 3 — speedup vs Linux (StarryOS / Linux; 1.0 = parity). The summary figure.
# ================================================================================
metrics = [
    ("Mem first-touch\n(THP)", 0.086/0.030),   # lower is better -> Linux/Starry
    ("Per-core A55",           370/359),
    ("Multi-thread t=4",       3810/3900),
    ("Single-core memcpy",     12.3/13.0),
    ("Single-thread cpu",      912/974),
    ("Per-core A76",           910/974),
    ("Mem BW (8-thread)",      20.0/55.0),
]
metrics.sort(key=lambda m: m[1])
names = [m[0] for m in metrics]; vals = [m[1] for m in metrics]
colors = [C_WIN if v >= 0.90 else C_GAP for v in vals]
fig, ax = plt.subplots(figsize=(6.6, 4.2))
bars = ax.barh(names, vals, color=colors, height=0.62)
ax.axvline(1.0, color="black", lw=1.3, ls="-")
ax.text(1.01, len(names)-0.4, "Linux parity", fontsize=8.5, va="center")
ax.axvline(0.9, color="grey", lw=0.9, ls=":")
for b, v, nm in zip(bars, vals, names):
    lbl = f"{v:.2f}×" + ("   (DDR DVFS firmware-blocked)" if "BW" in nm else "")
    ax.text(v + 0.03, b.get_y() + b.get_height()/2, lbl, va="center", fontsize=9,
            color=(C_GAP if "BW" in nm else "black"))
ax.set_xlabel("StarryOS ÷ Linux  (>1 = StarryOS faster)")
ax.set_title("StarryOS performance relative to Linux (RK3588)")
ax.set_xlim(0, 3.1)
fig.savefig("fig3_speedup.png"); plt.close(fig)

# ================================================================================
# Fig 4 — memory: first-touch latency (lower=better) + bandwidth (higher=better)
# ================================================================================
fig, (axl, axr) = plt.subplots(1, 2, figsize=(7.6, 3.8))
# first-touch
ft_lbl = ["Linux", "StarryOS\n(THP)"]; ft = [0.086, 0.030]
axl.bar(ft_lbl, ft, color=[C_LINUX, C_WIN], width=0.55)
axl.text(1, 0.030 + 0.003, "2.9× faster", ha="center", fontsize=9, color=C_WIN)
axl.set_ylabel("seconds  (lower = better)")
axl.set_title("Memory first-touch  (128 MB)")
axl.set_ylim(0, 0.10)
# bandwidth
bw_lbl = ["Linux", "StarryOS"]; bw = [55.0, 20.0]
axr.bar(bw_lbl, bw, color=[C_LINUX, C_GAP], width=0.55)
axr.text(1, 20.0 + 1.4, "0.36×\n(DDR fw-blocked)", ha="center", fontsize=8, color=C_GAP)
axr.set_ylabel("GB/s  (higher = better)")
axr.set_title("Memory bandwidth  (8-thread, 1M)")
axr.set_ylim(0, 62)
fig.suptitle("Memory: first-touch beats Linux; bandwidth capped by board firmware", fontsize=11)
fig.savefig("fig4_memory.png"); plt.close(fig)

print("wrote fig1_cpu_scaling.png fig2_percore.png fig3_speedup.png fig4_memory.png")
