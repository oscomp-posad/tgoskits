NAVY="#0D1B2A"; TEAL="#2DD4BF"; AMBER="#F59E0B"; INK="#EEF3F8"; MUTED="#8AA0B4"
import matplotlib as mpl, matplotlib.font_manager as fm
for _f in ("/System/Library/Fonts/Supplemental/Songti.ttc","/System/Library/Fonts/STHeiti Medium.ttc"):
    try: fm.fontManager.addfont(_f)
    except Exception: pass
_CJK="Songti SC"
# Force every chart to save at >=300 dpi (deck figures were shipping at 180-200
# dpi and looked blurry full-screen). Charts pass explicit dpi= to savefig, so
# bump it here rather than editing each script.
import matplotlib.figure as _mfig
_orig_savefig = _mfig.Figure.savefig
def _hidpi_savefig(self, *args, **kw):
    d = kw.get("dpi")
    if d is None or (isinstance(d, (int, float)) and d < 300):
        kw["dpi"] = 300
    return _orig_savefig(self, *args, **kw)
_mfig.Figure.savefig = _hidpi_savefig
def apply(dark=True):
    bg = NAVY if dark else "#FFFFFF"; fg = INK if dark else "#1a2230"
    mpl.rcParams.update({"figure.facecolor":bg,"axes.facecolor":bg,"savefig.facecolor":bg,
        "text.color":fg,"axes.edgecolor":MUTED,"axes.labelcolor":fg,"xtick.color":fg,
        "ytick.color":fg,"font.family":_CJK,"axes.unicode_minus":False,"axes.grid":True,
        "grid.color":MUTED,"grid.alpha":0.18,"axes.spines.top":False,"axes.spines.right":False})
BAR_ORDER=[TEAL,AMBER,MUTED]
