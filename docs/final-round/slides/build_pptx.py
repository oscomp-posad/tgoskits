#!/usr/bin/env python3
"""Build a presentable deck.pptx from the compiled Beamer deck, with the demo
GIFs laid directly over the terminal panels on their OWN existing slides so they
animate in place (PowerPoint / Keynote slideshow) — no separate GIF slides.

Every deck page becomes a full-bleed high-res image slide (preserves the LaTeX
design, stays sharp, no lossy compression). On the postgres slide (p17) and the
NPU-demo slide (p19) the matching GIF is overlaid exactly on the detected
terminal rectangle. Run from the repo root AFTER `make slides`:

    pdftoppm -r 300 -png slides/build/deck.pdf /tmp/deckpng/pg
    python3 slides/build_pptx.py

Requires python-pptx + Pillow. Output: slides/build/deck.pptx
"""
import glob
import os

from PIL import Image
from pptx import Presentation
from pptx.util import Emu, Inches

PNGDIR = "/tmp/deckpng"
DEMOS = "/Users/jsph273/Desktop/Code/tgoskits/.claude/worktrees/demos/demos/casts"
GIF_NPU = os.path.join(DEMOS, "qemu-npu-tennis.gif")
GIF_PG = os.path.join(DEMOS, "postgresql.gif")
GIF_PERF = "figures/out/Fperf_cast.gif"  # real board perf recording (repo-local)
OUT = "slides/build/deck.pptx"

# page (1-based) -> (gif, x-search-fraction range) for the in-place overlay
OVERLAYS = {
    12: (GIF_PERF, (0.02, 0.98)),  # perf recording, full width
    18: (GIF_PG, (0.02, 0.62)),    # postgres session, left column
    19: (GIF_NPU, (0.02, 0.60)),   # NPU inference, left column
}


def detect_terminal_box(png, xr):
    """Bounding box (px) of the dark neutral-grey terminal panel, restricted to
    the x-fraction range xr. The asciinema panel bg is neutral (~R=G=B) and dark,
    unlike the blue-tinted navy slide bg — that's the discriminator."""
    im = Image.open(png).convert("RGB")
    px = im.load()
    w, h = im.size
    x0, x1 = int(xr[0] * w), int(xr[1] * w)
    xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(x0, x1, 2):
            r, g, b = px[x, y]
            if max(r, g, b) < 60 and (b - r) < 12 and (max(r, g, b) - min(r, g, b)) < 16:
                xs.append(x)
                ys.append(y)
    if len(xs) < 500:
        return None
    xs.sort()
    ys.sort()
    q = lambda a, p: a[int(len(a) * p)]
    return q(xs, 0.005), q(ys, 0.005), q(xs, 0.995), q(ys, 0.995), w, h


prs = Presentation()
prs.slide_width = Inches(13.333)
prs.slide_height = Inches(7.5)
SW, SH = int(prs.slide_width), int(prs.slide_height)
blank = prs.slide_layouts[6]

pages = sorted(glob.glob(os.path.join(PNGDIR, "pg-*.png")))
if not pages:
    raise SystemExit(f"no page PNGs in {PNGDIR} — run pdftoppm -r 300 first")

for i, png in enumerate(pages, start=1):
    s = prs.slides.add_slide(blank)
    s.shapes.add_picture(png, 0, 0, width=SW, height=SH)
    if i in OVERLAYS:
        gif, xr = OVERLAYS[i]
        box = detect_terminal_box(png, xr)
        if box:
            bx0, by0, bx1, by1, pw, ph = box
            left = Emu(round(bx0 / pw * SW))
            top = Emu(round(by0 / ph * SH))
            wid = Emu(round((bx1 - bx0) / pw * SW))
            hei = Emu(round((by1 - by0) / ph * SH))
            s.shapes.add_picture(gif, left, top, width=wid, height=hei)
            print(f"page {i}: overlay {os.path.basename(gif)} @ "
                  f"({bx0/pw:.2f},{by0/ph:.2f})-({bx1/pw:.2f},{by1/ph:.2f})")
        else:
            print(f"page {i}: WARNING terminal box not detected — no overlay")

prs.save(OUT)
print(f"saved {OUT} — {len(prs.slides._sldIdLst)} slides, gifs overlaid in place")
