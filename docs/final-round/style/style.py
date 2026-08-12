NAVY="#0D1B2A"; TEAL="#2DD4BF"; AMBER="#F59E0B"; INK="#EEF3F8"; MUTED="#8AA0B4"
import matplotlib as mpl, matplotlib.font_manager as fm
for _f in ("/System/Library/Fonts/Supplemental/Songti.ttc","/System/Library/Fonts/STHeiti Medium.ttc"):
    try: fm.fontManager.addfont(_f)
    except Exception: pass
_CJK="Songti SC"
def apply(dark=True):
    bg = NAVY if dark else "#FFFFFF"; fg = INK if dark else "#1a2230"
    mpl.rcParams.update({"figure.facecolor":bg,"axes.facecolor":bg,"savefig.facecolor":bg,
        "text.color":fg,"axes.edgecolor":MUTED,"axes.labelcolor":fg,"xtick.color":fg,
        "ytick.color":fg,"font.family":_CJK,"axes.unicode_minus":False,"axes.grid":True,
        "grid.color":MUTED,"grid.alpha":0.18,"axes.spines.top":False,"axes.spines.right":False})
BAR_ORDER=[TEAL,AMBER,MUTED]
