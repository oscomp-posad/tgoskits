#!/usr/bin/env python3
"""Render the real perf CPU flame graph (tennis inference profile) to a tight
strip for the deck. Source SVG is the actual `perf script | flamegraph.pl`
output captured on the board (6735 samples). Run from the repo root:

    python3 figures/src/proofs/gen_perf_flame.py

Produces figures/out/Fperf_flame.png — the flame-bars band only (the title and
the SVG's internal whitespace are cropped; the slide frametitle supplies the
caption). Requires macOS `qlmanage` for SVG rasterization + Pillow.
"""
import os
import subprocess
import sys

from PIL import Image

SRC = "figures/src/proofs/flame_perf.svg"
OUT = "figures/out/Fperf_flame.png"
TMP = "/tmp/flame_perf.svg.png"
WIDTH = 2400  # rasterization width; flame is wide + shallow, so upscale for text


def render():
    subprocess.run(
        ["qlmanage", "-t", "-s", str(WIDTH), "-o", "/tmp", SRC],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if not os.path.exists(TMP):
        sys.exit("qlmanage produced no PNG (macOS-only tool)")
    return Image.open(TMP).convert("RGB")


def saturated_rows(im):
    """Row indices whose pixels contain large saturated color blocks (the flame
    bars) rather than sparse black title text or white background."""
    px = im.load()
    w, h = im.size
    rows = []
    step = max(1, w // 400)  # subsample columns for speed
    for y in range(h):
        count = 0
        for x in range(0, w, step):
            r, g, b = px[x, y]
            if max(r, g, b) - min(r, g, b) > 40 and max(r, g, b) > 90:
                count += 1
        if count > 60:  # a real flame row spans most of the width
            rows.append(y)
    return rows


def main():
    im = render()
    rows = saturated_rows(im)
    if not rows:
        sys.exit("no flame band detected")
    top, bot = min(rows), max(rows)
    pad = 8
    top = max(0, top - pad)
    bot = min(im.size[1], bot + pad)
    band = im.crop((0, top, im.size[0], bot))
    # trim left/right white margins
    gray = band.convert("L")
    bbox = gray.point(lambda v: 0 if v > 248 else 255).getbbox()
    if bbox:
        band = band.crop((bbox[0], 0, bbox[2], band.size[1]))
    band.save(OUT)
    print(f"wrote {OUT}  {band.size}")


if __name__ == "__main__":
    main()
