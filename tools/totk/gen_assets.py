#!/usr/bin/env python3
"""Draw the package's own UI shapes (no game art): the circular minimap mask and frame, and the
stamina ring frames. White on transparent; the manifest tints them.

    tools/totk/gen_assets.py packages/TearsOfTheKingdom/dualscreen/assets
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SS = 4  # supersampling for smooth edges


def smooth(size, draw_fn):
    big = Image.new("L", (size * SS, size * SS), 0)
    draw_fn(ImageDraw.Draw(big), size * SS)
    alpha = big.resize((size, size), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    out.putalpha(alpha)
    return out


def main(out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    Image.new("RGBA", (1, 1), (0, 0, 0, 0)).save(out / "map-transparent.png")

    # Opaque outside a circle inscribed in the square: covers the square map's corners.
    def outside(d, s):
        d.rectangle([0, 0, s, s], fill=255)
        d.ellipse([0, 0, s - 1, s - 1], fill=0)
    smooth(400, outside).save(out / "circle-outside.png", optimize=True)

    def ring(width):
        def draw(d, s):
            d.ellipse([0, 0, s - 1, s - 1], fill=255)
            w = width * SS
            d.ellipse([w, w, s - 1 - w, s - 1 - w], fill=0)
        return draw
    smooth(800, ring(3)).save(out / "circle-frame.png", optimize=True)

    # Vertical fade, opaque at the top: keeps a title readable over the map.
    fade = Image.new("RGBA", (4, 64), (255, 255, 255, 0))
    for y in range(64):
        a = int(255 * (1 - y / 63) ** 1.6)
        for x in range(4):
            fade.putpixel((x, y), (255, 255, 255, a))
    fade.save(out / "fade-down.png", optimize=True)

    # Stamina ring: 51 frames (0, 2, 4 ... 100 %), clockwise from the top, drawn around the
    # minimap frame. Frame 0 is empty.
    size, width = 820, 12
    for i in range(51):
        pct = i * 2

        def arc(d, s, pct=pct):
            if pct <= 0:
                return
            w = width * SS
            box = [w // 2, w // 2, s - 1 - w // 2, s - 1 - w // 2]
            end = -90 + 360 * pct / 100
            d.arc(box, -90, end, fill=255, width=w)
        smooth(size, arc).save(out / f"stamina-{i:02d}.png", optimize=True)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "packages/TearsOfTheKingdom/dualscreen/assets")
