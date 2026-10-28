#!/usr/bin/env python3
"""Rough offline preview of a DSMod manifest page, for layout work without the emulator.

Draws rects, bars, file: images (tinted) and text; game art (romfs:, module:, binds) becomes a
labelled placeholder box. Gates read from --true (names that are 1) and a default rule: binds
ending in valid/supported/show count as 1, everything else as 0. Text uses a stand-in italic
font, so sizes are close but not exact.

    tools/totk/preview_manifest.py manifest.json ready out.png [--true a,b] [--repeat 12]
"""
import argparse
import json
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
SAMPLE = {"bind_text": "Sample", "value": "120"}


def argb(s, default=(0, 0, 0, 0)):
    if not s:
        return default
    s = s.lstrip("#")
    if len(s) == 6:
        s = "FF" + s
    a, r, g, b = (int(s[i:i + 2], 16) for i in (0, 2, 4, 6))
    return (r, g, b, a)


class State:
    def __init__(self, true):
        self.true = set(true)

    def value(self, name, i):
        name = name.replace("{i}", str(i))
        neg = name.startswith("!")
        name = name.lstrip("!")
        if name in self.true:
            v = 1
        elif re.search(r"(valid|supported|show|\.module)$", name):
            v = 1
        else:
            v = 0
        return v, neg

    def shown(self, w, i):
        if "need_bind" in w:
            v, neg = self.value(w["need_bind"], i)
            if bool(v) == neg:
                return False
        if "hide_bind" in w:
            v, neg = self.value(w["hide_bind"], i)
            if "hide_eq" in w:
                if v == w["hide_eq"]:
                    return False
            elif "keep_min" in w or "keep_max" in w:
                lo = w.get("keep_min", -10**9)
                hi = w.get("keep_max", 10**9)
                lo = int(str(lo).replace("{i}", str(i))) if isinstance(lo, str) else lo
                hi = int(str(hi).replace("{i}", str(i))) if isinstance(hi, str) else hi
                if not lo <= v <= hi:
                    return False
        return True


def tinted(img, tint):
    r, g, b, a = argb(tint, (255, 255, 255, 255))
    if (r, g, b, a) == (255, 255, 255, 255):
        return img
    px = img.load()
    out = img.copy()
    po = out.load()
    for y in range(img.height):
        for x in range(img.width):
            pr, pg, pb, pa = px[x, y]
            po[x, y] = (pr * r // 255, pg * g // 255, pb * b // 255, pa * a // 255)
    return out


def render(manifest_path, page_id, out, true=(), repeat_max=12):
    root = Path(manifest_path).parent
    m = json.loads(Path(manifest_path).read_text())
    canvas = Image.new("RGBA", (m["canvas_w"], m["canvas_h"]), argb(m.get("background"), (0, 0, 0, 255)))
    page = next(p for p in m["pages"] if p["id"] == page_id)
    state = State(true)
    cache = {}

    def paste(layer):
        canvas.alpha_composite(layer)

    for w in page["widgets"]:
        n = min(w.get("repeat", 1), repeat_max)
        cols = w.get("repeat_cols", n)
        for i in range(n):
            if not state.shown(w, i):
                continue
            x, y, ww, hh = (float(v) for v in w["rect"])
            if n > 1:
                x += (i % cols) * w.get("repeat_dx", 0)
                y += (i // cols) * w.get("repeat_row_dy", 0)
            layer = Image.new("RGBA", canvas.size)
            d = ImageDraw.Draw(layer)
            t = w["type"]
            box = [x, y, x + ww - 1, y + hh - 1]
            if t == "rect":
                if argb(w.get("bg"))[3]:
                    d.rectangle(box, fill=argb(w["bg"]))
                fr = w.get("frame", 1 if argb(w.get("color"))[3] else 0)
                if fr and argb(w.get("color"))[3]:
                    d.rectangle(box, outline=argb(w["color"]), width=int(fr))
            elif t == "bar":
                d.rectangle(box, fill=argb(w.get("bg")))
                d.rectangle([x, y, x + ww * 0.7, y + hh - 1], fill=argb(w.get("color")))
            elif t == "map":
                d.rectangle(box, fill=(70, 62, 40, 255))
                d.text((x + 10, y + 10), "map", fill=(200, 200, 200, 255))
            elif t == "image":
                src = w.get("src", "")
                if src.startswith("file:") and (root / src[5:]).exists():
                    key = (src, int(ww), int(hh), w.get("tint"), w.get("rotate", 0))
                    if key not in cache:
                        im = Image.open(root / src[5:]).convert("RGBA")
                        if w.get("rotate"):
                            im = im.rotate(-w["rotate"], expand=True)
                        im = im.resize((max(1, int(ww)), max(1, int(hh))))
                        cache[key] = tinted(im, w.get("tint"))
                    layer.alpha_composite(cache[key], (int(x), int(y)))
                elif "src_names" in w and (root / w["src_names"][-1][5:]).exists():
                    im = Image.open(root / w["src_names"][len(w["src_names"]) * 2 // 3][5:]).convert("RGBA")
                    layer.alpha_composite(tinted(im.resize((int(ww), int(hh))), w.get("tint")), (int(x), int(y)))
                else:
                    name = (src or w.get("src_bind") or w.get("bind") or "?").split("#")[-1].split("/")[-1]
                    col = argb(w.get("tint"), (255, 255, 255, 255))
                    d.rectangle(box, outline=(col[0], col[1], col[2], 140), width=2)
                    d.line([x, y, x + ww, y + hh], fill=(col[0], col[1], col[2], 60))
                    d.text((x + 4, y + 4), name[:14], fill=(255, 255, 255, 160))
            elif t in ("label", "value"):
                text = w.get("text") or ""
                if t == "value":
                    text = SAMPLE["value"] + (w.get("suffix") or "")
                elif w.get("bind_text") or w.get("text_bind"):
                    text = "88" if w.get("text_role") == "numeral_readout" else "Sample text"
                if not text:
                    continue
                size = max(8, int(w.get("text_scale", 3) * 6.6))
                font = ImageFont.truetype(FONT, size)
                align = w.get("align", "left")
                tw = d.textlength(text, font=font)
                ax = x - tw / 2 if align == "center" else x - tw if align == "right" else x
                ay = y
                if w.get("text_center_h"):
                    ay = y + (w["text_center_h"] - size) / 2
                if w.get("outline"):
                    d.text((ax, ay), text, font=font, fill=argb(w.get("color"), (255, 255, 255, 255)),
                           stroke_width=int(w.get("outline_px", 2)), stroke_fill=argb(w["outline"]))
                else:
                    d.text((ax, ay), text, font=font, fill=argb(w.get("color"), (255, 255, 255, 255)))
            paste(layer)
    canvas.convert("RGB").save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest")
    ap.add_argument("page")
    ap.add_argument("out")
    ap.add_argument("--true", default="")
    ap.add_argument("--repeat", type=int, default=12)
    a = ap.parse_args()
    render(a.manifest, a.page, a.out, [s for s in a.true.split(",") if s], a.repeat)


if __name__ == "__main__":
    main()
