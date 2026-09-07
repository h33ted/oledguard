#!/usr/bin/env python3
"""Rasterise dmg-background.svg into the two scales the disk image needs.

    python3 packaging/macos/make_dmg_bg.py

Writes packaging/macos/dmg/background.png (640x400) and background@2x.png
(1280x800). make_dmg.sh combines them into a single multi-resolution TIFF with
tiffutil, which is how Finder picks the right one on a Retina display.

Both PNGs are committed, so a plain `sh make_dmg.sh` needs nothing but macOS.
Re-run this only after editing the SVG.
"""
import os
import sys

try:
    import cairosvg
except ImportError:
    sys.exit("needs cairosvg: pip install cairosvg")

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "dmg-background.svg")
OUT = os.path.join(HERE, "dmg")

W, H = 640, 400

os.makedirs(OUT, exist_ok=True)
for name, scale in (("background.png", 1), ("background@2x.png", 2)):
    path = os.path.join(OUT, name)
    cairosvg.svg2png(url=SVG, write_to=path,
                     output_width=W * scale, output_height=H * scale)
    print("wrote", os.path.relpath(path, os.path.dirname(HERE)),
          "%dx%d" % (W * scale, H * scale))
