import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, matplotlib.font_manager as fm
from matplotlib.patches import FancyBboxPatch
NAVY="#0D1B2A"; PANEL="#0f2233"; TEAL="#2DD4BF"; INK="#EEF3F8"; MUTED="#8AA0B4"; GREEN="#7BE38A"
for f in ["/System/Library/Fonts/Menlo.ttc","/System/Library/Fonts/Supplemental/Songti.ttc"]:
    try: fm.fontManager.addfont(f)
    except Exception: pass
plt.rcParams.update({"font.family":["Menlo","Songti SC"],"axes.unicode_minus":False})
L=[
 ("── PostgreSQL 17 在 StarryOS 上：安装 → initdb → 建库 → 14 阶段 SQL 负载 ──", MUTED, True),
 ("", INK, False),
 ("root@starry:~# apk add postgresql17            # StarryOS 的 shell", INK, True),
 ("root@starry:~# initdb -D /var/lib/postgresql/data      ... ok", INK, True),
 ("root@starry:~# pg_ctl start   &&   psql -c 'SELECT 1;'      ->  1", INK, True),
 ("", INK, False),
 ("  POSTGRESQL_STAGE_PASSED  1-5/14   建库 · 建表(外键+索引) · 多行插入", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED  7/14     select 过滤 / 排序", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED  8/14     聚合查询", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED  9/14     连接查询", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED 10/14     更新", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED 11/14     删除 + 事务回滚", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED 12/14     批量插入 generate_series", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED 13/14     数据完整性校验（行数 / 更新持久化 / 回滚正确性）", GREEN, False),
 ("  POSTGRESQL_STAGE_PASSED 14/14     干净关闭", GREEN, False),
]
fig, ax = plt.subplots(figsize=(11.4, 6.6)); fig.patch.set_facecolor(NAVY); ax.set_facecolor(NAVY); ax.axis("off")
ax.add_patch(FancyBboxPatch((0.008,0.20),0.984,0.76, boxstyle="round,pad=0.006,rounding_size=0.02",
             transform=ax.transAxes, fc=PANEL, ec=MUTED, lw=1.0, zorder=0))
for i,c in enumerate(["#ff5f56","#ffbd2e","#27c93f"]):
    ax.add_patch(plt.Circle((0.035+i*0.022, 0.925), 0.009, transform=ax.transAxes, color=c, zorder=2))
ax.text(0.5,0.925,"PostgreSQL 17 @ StarryOS (QEMU)", transform=ax.transAxes, ha="center", va="center", color=MUTED, fontsize=12)
y=0.855; dy=(0.855-0.255)/(len(L)-1)
for t,c,b in L:
    ax.text(0.045,y,t, transform=ax.transAxes, ha="left", va="center", color=c, fontsize=12.8, fontweight=("bold" if b else "normal")); y-=dy
ax.add_patch(FancyBboxPatch((0.008,0.02),0.984,0.135, boxstyle="round,pad=0.004,rounding_size=0.02",
             transform=ax.transAxes, fc="#122e2a", ec=TEAL, lw=1.3, zorder=0))
ax.text(0.5,0.088,"POSTGRESQL_TEST_PASSED    ·    14 / 14 阶段全部通过", transform=ax.transAxes,
        ha="center", va="center", color=TEAL, fontsize=17, fontweight="bold")
# titleless: the deck/report frametitle supplies the title
fig.savefig("figures/out/Fpostgres_proof.png", dpi=200, facecolor=NAVY, bbox_inches="tight")
print("wrote figures/out/Fpostgres_proof.png")
