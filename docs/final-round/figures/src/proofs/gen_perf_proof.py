import sys, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.font_manager as fm
NAVY="#0D1B2A"; PANEL="#0f2233"; TEAL="#2DD4BF"; AMBER="#F59E0B"; INK="#EEF3F8"; MUTED="#8AA0B4"; GREEN="#7BE38A"
# monospace with CJK fallback
for f in ["/System/Library/Fonts/Menlo.ttc","/System/Library/Fonts/Supplemental/Songti.ttc","/System/Library/Fonts/STHeiti Medium.ttc"]:
    try: fm.fontManager.addfont(f)
    except Exception: pass
plt.rcParams.update({"font.family":["Menlo","Songti SC"],"axes.unicode_minus":False})

# (text, color, bold)  — lines of a terminal session, real board captures
P=TEAL  # prompt
L=[
 ("── 未改动的上游 perf 6.1.0 直接在 StarryOS 真机上运行 ──", MUTED, True),
 ("", INK, False),
 ("root@starry:~# perf stat true", INK, True),
 ("        19,300,000      cycles          # 真实硬件计数", INK, False),
 ("             IPC 0.91", GREEN, False),
 ("", INK, False),
 ("root@starry:~# perf record -g -- <workload>     [ 751 samples · perf.data 0.031 MB ]", INK, True),
 ("root@starry:~# perf report --stdio", INK, True),
 ("    7.73%  [kernel]  UserContext::run", INK, False),
 ("    5.57%  [kernel]  memcpy", INK, False),
 ("    3.02%  [kernel]  on_exec_sideband        (符号经 /proc/kallsyms 解析)", MUTED, False),
 ("root@starry:~# perf top --stdio      drop 0/10115 (4 CPUs)", INK, True),
 ("  100.00%  [kernel]  [k] ax_task::run_idle", GREEN, False),
 ("", INK, False),
 ("root@starry:~# perf record -e probe:hsc -- cmd      # kprobe 动态跟踪", INK, True),
 ("    284 samples in 11.97s     perf report: # Samples: 284 of event 'probe:hsc'", GREEN, False),
 ("root@starry:~# ftrace  function tracer   11,512 patchable entries · 73x handle_syscall", INK, True),
 ("                kprobe(tracefs)          73x hsc(0x...) records", MUTED, False),
]
fig, ax = plt.subplots(figsize=(11.4, 6.6))
fig.patch.set_facecolor(NAVY); ax.set_facecolor(NAVY); ax.axis("off")
# rounded panel
from matplotlib.patches import FancyBboxPatch
ax.add_patch(FancyBboxPatch((0.008,0.16),0.984,0.80, boxstyle="round,pad=0.006,rounding_size=0.02",
             transform=ax.transAxes, fc=PANEL, ec=MUTED, lw=1.0, zorder=0))
# title bar dots
for i,c in enumerate(["#ff5f56","#ffbd2e","#27c93f"]):
    ax.add_patch(plt.Circle((0.035+i*0.022, 0.925), 0.009, transform=ax.transAxes, color=c, zorder=2))
ax.text(0.5, 0.925, "perf @ OrangePi-5-Plus (RK3588, 4×A55 + 4×A76)", transform=ax.transAxes,
        ha="center", va="center", color=MUTED, fontsize=12)
y=0.855; dy=(0.855-0.205)/(len(L)-1)
for t,c,b in L:
    ax.text(0.045, y, t, transform=ax.transAxes, ha="left", va="center",
            color=c, fontsize=12.3, fontweight=("bold" if b else "normal"))
    y-=dy
# footer highlight strip
ax.add_patch(FancyBboxPatch((0.008,0.015),0.984,0.115, boxstyle="round,pad=0.004,rounding_size=0.02",
             transform=ax.transAxes, fc="#122e2a", ec=TEAL, lw=1.2, zorder=0))
ax.text(0.5,0.087,"big.LITTLE PMU 矩阵  39 / 0 通过        perf_event_open 特性矩阵  56 / 0 通过",
        transform=ax.transAxes, ha="center", va="center", color=TEAL, fontsize=14, fontweight="bold")
ax.text(0.5,0.042,"A55(type 9)/A76(type 10) 双簇独立编程 · 用户态 160000055 vs 内核态 28455（5600× exclude-bit 过滤验证）",
        transform=ax.transAxes, ha="center", va="center", color=MUTED, fontsize=10.5)
fig.suptitle("硬件 PMU perf：在真机上跑通 stat / record / report / top / kprobe / ftrace",
             color=INK, fontsize=16.5, fontweight="bold", y=0.985)
fig.savefig("figures/out/Fperf_proof.png", dpi=200, facecolor=NAVY, bbox_inches="tight")
print("wrote figures/out/Fperf_proof.png")
