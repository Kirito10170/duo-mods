#!/usr/bin/env python3
"""Generate packages/TearsOfTheKingdom/dualscreen/manifest.json.

The look follows igawa6's Breath of the Wild companion: a dark slate page, cream text, framed
dark tiles, a blue accent and a bottom command bar. Game art and the game font are added once
the module decodes them from the player's romfs (TOTK's romfs is zstd-compressed, which the
host's romfs: sources do not read).

    tools/totk/gen_manifest.py > packages/TearsOfTheKingdom/dualscreen/manifest.json
"""
import json

W, H = 1240, 1080
BG = "#FF0C0F11"
HEADER = "#E6141A1C"
CREAM = "#FFFFFCC6"
CREAM_DIM = "#9EE6ECF2"
RULE = "#50FFFCC6"
TILE_BG = "#C8000000"
TILE_FRAME = "#3CFFFCC6"
ACCENT = "#FF0090FD"
TITLE_BLUE = "#FF5FC8F0"
BUTTON_BG = "#FF0A0A05"
BUTTON_FRAME = "#FF4A4630"
CLEAR = "#00000000"

NAV = [("map", "Map"), ("inventory", "Inventory"), ("options", "Settings")]
CATEGORIES = ["Weapons", "Bows", "Shields", "Armor", "Materials", "Food", "Key Items"]
OPTIONS = ["Camera Sensitivity", "Invert Vertical Axis", "Invert Horizontal Axis",
           "Aiming Controls", "HUD Mode", "Map Orientation", "Text Speed"]
# Published by the module, which the host loads only on the supported build (module.build_ids).
CONTENT_GATE = "totk.supported"


def rect(r, bg, color=CLEAR, **kw):
    return {"type": "rect", "rect": r, "bg": bg, "color": color, **kw}


def label(r, text, scale=3, color=CREAM, **kw):
    # A label draws from an anchor at rect[0]: "center" centres the text on it and "right" ends
    # the text there; the rect width is ignored. Move the anchor so r reads as a box.
    x, y, w, h = r
    align = kw.get("align")
    if align == "center":
        x += w // 2
    elif align == "right":
        x += w
    return {"type": "label", "rect": [x, y, w, h], "text": text, "text_scale": scale,
            "color": color, **kw}


def button(r, text, action, active=False):
    return [rect(r, BUTTON_BG, BUTTON_FRAME, frame=3, on_tap=action),
            label([r[0], r[1], r[2], r[3]], text, 5, TITLE_BLUE if active else CREAM,
                  align="center", text_center_h=r[3])]


def command_bar(active, extra=()):
    """Bottom bar: the three pages, then page-specific buttons on the right."""
    w = [rect([0, 950, W, 130], HEADER), rect([24, 950, 1192, 1], RULE)]
    x = 24
    for pid, text in NAV:
        w += button([x, 966, 236, 98], text, "go_" + pid, pid == active)
        x += 248
    x = W - 24
    for text, action in reversed(list(extra)):
        x -= 220
        w += button([x, 966, 220, 98], text, action)
        x -= 12
    return w


def unsupported_notice():
    return label([60, 420, 1120, 200],
                 "This companion needs Tears of the Kingdom version 1.4.2. Update the game to use it.",
                 4, "#FFFFB74D", wrap_width=1120, align="center", hide_bind=CONTENT_GATE)


def map_page():
    w = [rect([0, 0, W, H], BG),
         rect([0, 0, W, 950], "#FF16202A"),
         label([0, 24, W, 80], "Surface", 6, TITLE_BLUE, align="center", need_bind=CONTENT_GATE),
         label([0, 440, W, 60], "Map in progress", 4, CREAM_DIM, align="center",
               need_bind=CONTENT_GATE),
         label([0, 900, W, 40], "Pinch to zoom - Drag to pan", 3, CREAM_DIM, align="center",
               need_bind=CONTENT_GATE)]
    w += command_bar("map", [("Layers", "map_layers")])
    return {"id": "map", "title": "Map", "widgets": w + [unsupported_notice()]}


def inventory_page():
    w = [rect([0, 0, W, H], BG), rect([0, 0, W, 124], HEADER)]
    cell = 112
    for i, name in enumerate(CATEGORIES):
        x = 24 + i * cell
        w.append(rect([x, 8, cell, cell], CLEAR, frame=0, on_tap=f"inventory_category_{i}",
                      need_bind=CONTENT_GATE))
        w.append(label([x, 8, cell, cell], name, 2, CREAM_DIM, align="center",
                       text_center_h=cell, need_bind=CONTENT_GATE))
    w.append(label([800, 20, 416, 80], "Weapons", 5, CREAM, align="right", text_center_h=80,
                   need_bind=CONTENT_GATE))
    w.append(rect([24, 128, 1192, 2], RULE))
    # 5 x 5 item grid, same pitch as the BOTW companion
    w.append(rect([24, 141, 140, 150], TILE_BG, TILE_FRAME, frame=1, repeat=25, repeat_cols=5,
                  repeat_dx=148, repeat_row_dy=160, need_bind=CONTENT_GATE))
    # detail panel with the blue accent line
    w.append(rect([780, 141, 436, 800], "#FF050708", need_bind=CONTENT_GATE))
    w.append(rect([788, 141, 6, 800], ACCENT, need_bind=CONTENT_GATE))
    w.append(label([824, 520, 380, 60], "Inventory in progress", 4, CREAM,
                   need_bind=CONTENT_GATE))
    w.append(rect([824, 600, 384, 2], RULE, need_bind=CONTENT_GATE))
    w.append(label([824, 620, 380, 200], "Items appear here once the reader is wired up.", 3,
                   CREAM_DIM, wrap_width=380, need_bind=CONTENT_GATE))
    w += command_bar("inventory", [("Sort", "inventory_sort")])
    return {"id": "inventory", "title": "Inventory", "widgets": w + [unsupported_notice()]}


def options_page():
    w = [rect([0, 0, W, H], BG), rect([0, 0, W, 124], HEADER),
         label([40, 20, 800, 80], "Options", 6, CREAM, text_center_h=80),
         rect([24, 128, 1192, 2], RULE)]
    for i, name in enumerate(OPTIONS):
        y = 150 + i * 108
        w.append(rect([24, y, 1192, 96], TILE_BG, TILE_FRAME, frame=1, need_bind=CONTENT_GATE))
        w.append(label([56, y, 700, 96], name, 4, CREAM, text_center_h=96,
                       need_bind=CONTENT_GATE))
        w.append(label([800, y, 384, 96], "—", 4, CREAM_DIM, align="right", text_center_h=96,
                       need_bind=CONTENT_GATE))
    w += command_bar("options")
    return {"id": "options", "title": "Settings", "widgets": w + [unsupported_notice()]}


def main():
    actions = {f"go_{pid}": {"kind": "page", "page": pid, "transition": "fade", "duration_ms": 150}
               for pid, _ in NAV}
    # Handled by the module once the readers exist; until then it declines them.
    for name in ["map_layers", "inventory_sort"] + [f"inventory_category_{i}" for i in range(len(CATEGORIES))]:
        actions[name] = {"kind": "module", "action": name, "argument": 0}
    manifest = {
        "format": 1,
        "title_id": "0100F2C0115B6000",
        "name": "Tears of the Kingdom companion",
        "min_runtime": 22,
        "_note": "Generated by tools/totk/gen_manifest.py. totk.supported comes from the module, "
                 "which only loads on 1.4.2; page values come from the module once located.",
        "requires_module": True,
        "canvas_w": W,
        "canvas_h": H,
        "background": BG,
        "actions": actions,
        "module_outputs": ["totk.supported", "totk.unsupported", "totk.ready"],
        "pages": [map_page(), inventory_page(), options_page()],
    }
    print(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
