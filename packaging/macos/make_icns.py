#!/usr/bin/env python3
"""Build every form of the OLEDGuard app icon from the SVG sources.

    python3 packaging/macos/make_icns.py

Produces, all under packaging/macos/:

  OLEDGuard.icns              classic icon, read by every macOS before 26
  OLEDGuard.iconset/          the same PNGs, for `iconutil` if preferred
  layers/                     background and foreground plates to re-import
                              into Icon Composer after an artwork change

The layered AppIcon.icon built in Icon Composer is what macOS 26 renders, and
it is edited there rather than generated here. Re-run this script after
changing an SVG, then drag the two files from layers/ back into Icon Composer
to refresh it.

Flattened dark and tinted variants are deliberately not produced any more.
They belong to the older asset-catalog mechanism, and feeding an already-dark
composite to Icon Composer, which applies its own dark treatment, darkens it
twice. See ICON.md.
"""
import os
import struct
import sys

import cairosvg

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "icon.svg")
SVG_SMALL = os.path.join(HERE, "icon-small.svg")
SVG_BG = os.path.join(HERE, "icon-bg.svg")

ICNS = os.path.join(HERE, "OLEDGuard.icns")
ICONSET = os.path.join(HERE, "OLEDGuard.iconset")
LAYERS = os.path.join(HERE, "layers")

# Below this the detailed artwork stops resolving; icon-small.svg takes over.
SMALL_MAX = 64

CHUNKS = [
    ("icp4", 16), ("icp5", 32),
    ("ic11", 32), ("ic12", 64),
    ("ic07", 128), ("ic13", 256),
    ("ic08", 256), ("ic14", 512),
    ("ic09", 512), ("ic10", 1024),
]

ICONSET_FILES = [
    ("icon_16x16.png", 16), ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256), ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512), ("icon_512x512@2x.png", 1024),
]



def load(path):
    with open(path, "r") as f:
        return f.read()




def render(svg_text, size):
    return cairosvg.svg2png(bytestring=svg_text.encode("utf-8"),
                            output_width=size, output_height=size)


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def build_icns(detailed, small):
    cache = {}
    for size in sorted({s for _, s in CHUNKS}):
        cache[size] = render(small if size <= SMALL_MAX else detailed, size)

    os.makedirs(ICONSET, exist_ok=True)
    for name, size in ICONSET_FILES:
        write(os.path.join(ICONSET, name), cache[size])

    body = b"".join(
        kind.encode("ascii") + struct.pack(">I", len(cache[size]) + 8) + cache[size]
        for kind, size in CHUNKS
    )
    write(ICNS, b"icns" + struct.pack(">I", len(body) + 8) + body)
    print("wrote OLEDGuard.icns and OLEDGuard.iconset/ (%d sizes)" % len(cache))




def build_layers(detailed):
    """Separate plates for Icon Composer, which composes and glazes them."""
    os.makedirs(LAYERS, exist_ok=True)
    # Foreground: hide the background group, keep the transparency.
    fg = detailed.replace('<g id="bglayer">', '<g id="bglayer" display="none">')
    write(os.path.join(LAYERS, "foreground.png"), render(fg, 1024))
    # Background: full-bleed and square; Icon Composer applies its own mask.
    write(os.path.join(LAYERS, "background.png"), render(load(SVG_BG), 1024))
    print("wrote layers/foreground.png and layers/background.png")


def main():
    for path in (SVG, SVG_SMALL, SVG_BG):
        if not os.path.exists(path):
            sys.exit("missing " + path)

    detailed = load(SVG)
    small = load(SVG_SMALL)

    build_icns(detailed, small)
    build_layers(detailed)


if __name__ == "__main__":
    main()
