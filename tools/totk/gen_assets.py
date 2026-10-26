#!/usr/bin/env python3
"""Draw the package's own UI shapes (no game art): the page light, dial discs, the selected-cell
bracket and the stamina ring frames. White on transparent; the manifest tints them. The frames,
masks and command bars copied from the BOTW companion are listed in ASSETS.provenance.json.

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

    # The page's soft top light (BOTW draws its Gradation128 texture there): opaque at the top.
    lift = Image.new("RGBA", (1240, 520), (255, 255, 255, 0))
    for y in range(520):
        a = int(255 * (1 - y / 519) ** 1.6)
        lift.paste((255, 255, 255, a), (0, y, 1240, y + 1))
    lift.save(out / "page-lift.png", optimize=True)

    # Dial discs (BOTW draws the game's Nt_Circle64 at these sizes).
    for size in (84, 80):
        smooth(size, lambda d, s: d.ellipse([0, 0, s - 1, s - 1], fill=255)).save(
            out / f"disc-{size}.png", optimize=True)

    # A selected cell's corner bracket, top-left; the manifest rotates it for the others.
    def bracket(d, s):
        o, t, n = s * 33 // 67, s * 5 // 67, s * 22 // 67
        d.rectangle([o - t // 2, o - t // 2, o + n, o + t // 2], fill=255)
        d.rectangle([o - t // 2, o - t // 2, o + t // 2, o + n], fill=255)
    smooth(67, bracket).save(out / "cell-corner.png", optimize=True)

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
