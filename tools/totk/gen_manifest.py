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
TILE_BG_SOLID = "#FF0A0B0C"
TILE_FRAME_GOLD = "#FF4A4630"
PANEL_BG = "#FF050708"
STAMINA_GREEN = "#FF6CE07A"
HEART_RED = "#FFE8434F"
TILE_FRAME = "#3CFFFCC6"
ACCENT = "#FF0090FD"
TITLE_BLUE = "#FF5FC8F0"
BUTTON_BG = "#FF0A0A05"
BUTTON_FRAME = "#FF4A4630"
CLEAR = "#00000000"

NAV = [("ready", "Ready"), ("map", "Map"), ("inventory", "Inventory"), ("options", "Settings")]  # page ids
CATEGORIES = ["Weapons", "Bows", "Shields", "Armor", "Materials", "Food", "Key Items"]
# The game's System > Options list (amiibo, a toggle with no value, is left out).
OPTIONS = ["Invert Vertical Camera", "Invert Horizontal Camera", "Camera Sensitivity",
           "Aim with Motion Controls", "Invert Vertical Ability Controls",
           "Invert Horizontal Ability Controls", "Swap Jump Controls", "Lock Minimap to North",
           "HUD Mode", "Message Window", "Voice Language"]
# Published by the module, which the host loads only on the supported build (module.build_ids).
CONTENT_GATE = "totk.supported"
# The world map: 12 x 10 cells of 1000 m, x east, map y = -z (north up). Cell i = row * 12 + col,
# row 0 the northern edge; the module publishes each cell's tile key for the layer on show.
MAP_MIN, MAP_MAX, CELL, COLS, ROWS = (-6000, -5000), (6000, 5000), 1000, 12, 10
# Link's marker; the module serves the arrow from the game's layout archive.
PLAYER_MARKER = "module:totk:player_marker"


def map_overlays(base, detail=None):
    out = []
    for i in range(COLS * ROWS):
        col, row = i % COLS, i // COLS
        o = {"src_bind": f"totk.map.tile.{base}.{i}",
             "x0": MAP_MIN[0] + col * CELL, "x1": MAP_MIN[0] + (col + 1) * CELL,
             "y0": MAP_MAX[1] - (row + 1) * CELL, "y1": MAP_MAX[1] - row * CELL}
        if detail is not None:
            o["src_detail_bind"] = f"totk.map.tile.{detail}.{i}"
            o["detail_threshold"] = 210
        out.append(o)
    return out


def map_widget(r, wid, **kw):
    return {"type": "map", "id": wid, "area": "Hyrule", "rect": r, "area_label": False,
            "marker_x_bind": "totk.map.x", "marker_y_bind": "totk.map.y",
            "marker_src": PLAYER_MARKER, "need_bind": "totk.map.valid", **kw}


def rect(r, bg, color=CLEAR, **kw):
    return {"type": "rect", "rect": r, "bg": bg, "color": color, **kw}


def image(r, src, **kw):
    return {"type": "image", "rect": r, "src": src, **kw}


def label(r, text, scale=3, color=CREAM, upright=False, **kw):
    # A label draws from an anchor at rect[0]: "center" centres the text on it and "right" ends
    # the text there; the rect width is ignored. Move the anchor so r reads as a box.
    x, y, w, h = r
    align = kw.get("align")
    if align == "center":
        x += w // 2
    elif align == "right":
        x += w
    # Menu text leans like the game's (and the BOTW companion's) italic UI face.
    style = {} if upright else {"italic": True, "slant_deg": 9.0}
    return {"type": "label", "rect": [x, y, w, h], "text": text, "text_scale": scale,
            "color": color, **style, **kw}


def button(r, text, action, active=False):
    return [rect(r, BUTTON_BG, BUTTON_FRAME, frame=3, on_tap=action),
            label([r[0], r[1], r[2], r[3]], text, 5, TITLE_BLUE if active else CREAM,
                  align="center", text_center_h=r[3])]


BAR_Y, BAR_H = 952, 112


def bottom_bar(buttons, right=(), hint=None):
    """BOTW-style command row: framed buttons on the left (or centred on Ready) and right."""
    w = []
    x = 24
    for text, action in buttons:
        w += button([x, BAR_Y, 222, BAR_H], text, action)
        x += 240
    if hint:
        w.append(label([x, BAR_Y, W - 24 - 240 * len(right) - x, BAR_H], hint, 3, CREAM,
                       align="center", text_center_h=BAR_H, outline="#C8000A19", outline_px=2))
    x = W - 24
    for text, action in reversed(list(right)):
        x -= 222
        w += button([x, BAR_Y, 222, BAR_H], text, action)
        x -= 18
    return w


def unsupported_notice():
    return label([60, 420, 1120, 200],
                 "This companion needs Tears of the Kingdom version 1.4.2. Update the game to use it.",
                 4, "#FFFFB74D", wrap_width=1120, align="center", hide_bind=CONTENT_GATE)


def gear_tile(r, slot, title, bar_top=True):
    """A BOTW-style gear tile: slot, item name, durability number."""
    x, y, w, h = r
    g = {"need_bind": CONTENT_GATE}
    out = [rect(r, TILE_BG_SOLID, TILE_FRAME_GOLD, frame=3, **g)]
    out.append(label([x + 10, y + 32, w - 20, 28], title, 2, CREAM_DIM, align="center", **g))
    out.append(label([x + 10, y + 66, w - 20, 72], "", 3, CREAM, align="center",
                     wrap_width=w - 20, max_lines=2, text_bind=f"totk.ready.{slot}.name", **g))
    out.append(label([x, y + h - 58, w, 50], "", 5, "#FFFFFFFF", upright=True, align="center",
                     text_center_h=50, text_bind=f"totk.ready.{slot}.durability_text", **g))
    out.append(rect(r, CLEAR, frame=0, on_tap="go_inventory"))
    return out


def side_panel(r, accent_left=True):
    x, y, w, h = r
    out = [rect(r, PANEL_BG, need_bind=CONTENT_GATE)]
    ax = x if accent_left else x + w - 6
    out.append(rect([ax, y, 6, h], ACCENT, need_bind=CONTENT_GATE))
    return out


MINIMAP = [220, 64, 800, 800]


def ready_page():
    """The BOTW companion's Ready page, rebuilt for TOTK: minimap with the stamina ring, gear in
    the corners, vitals and abilities on the sides."""
    g = {"need_bind": CONTENT_GATE}
    w = [rect([0, 0, W, H], BG)]
    # Minimap: the square map widget, masked to a circle, framed, ringed by stamina.
    mx, my, mw, mh = MINIMAP
    w.append(image(MINIMAP, "file:assets/circle-outside.png", tint="#FF000D17", **g))
    w.append(map_widget(MINIMAP, "ready_map", follow_window=1100,
                        overlays=map_overlays("near"), marker_size=[84, 84]))
    w.append(image(MINIMAP, "file:assets/circle-outside.png", tint=BG, **g))
    w.append(image(MINIMAP, "file:assets/circle-frame.png", tint="#6EFFFCC6", **g))
    w.append({"type": "image", "rect": [mx - 10, my - 10, mw + 20, mh + 20],
              "bind": "totk.ready.stamina_frame",
              "src_names": [f"file:assets/stamina-{i:02d}.png" for i in range(51)],
              "tint": STAMINA_GREEN, **g})
    w.append(label([mx, my + mh + 18, mw, 56], "", 4, TITLE_BLUE, align="center",
                   text_center_h=56, text_bind="totk.map.layer_name", **g))
    # Corners: shield and bow on top, battery and weapon at the bottom (BOTW's places).
    w += gear_tile([18, 18, 180, 190], "shield", "Shield")
    w += gear_tile([1042, 18, 180, 190], "bow", "Bow")
    w += gear_tile([1042, 742, 180, 190], "weapon", "Weapon")
    # Left: hearts.
    w += side_panel([18, 232, 180, 300])
    w.append(label([30, 252, 160, 40], "Hearts", 3, CREAM_DIM, align="center", **g))
    w.append(label([24, 300, 168, 90], "", 8, "#FFFFFFFF", upright=True, align="center",
                   text_center_h=90, text_bind="totk.ready.hearts", **g))
    w.append(label([30, 400, 160, 40], "", 3, CREAM_DIM, align="center",
                   text_bind="totk.ready.hearts_of", **g))
    w.append({"type": "bar", "rect": [36, 460, 144, 10], "bind": "totk.ready.life_pct",
              "max": 100, "bg": "#40FFFCC6", "color": HEART_RED, "frame": 0, **g})
    # Left bottom: Zonai battery.
    w += side_panel([18, 552, 180, 380])
    w.append(label([30, 572, 160, 40], "Battery", 3, CREAM_DIM, align="center", **g))
    w.append(label([24, 630, 168, 90], "", 7, "#FFFFFFFF", upright=True, align="center",
                   text_center_h=90, text_bind="totk.ready.battery_text", **g))
    w.append({"type": "bar", "rect": [36, 740, 144, 10], "bind": "totk.ready.battery_pct",
              "max": 100, "bg": "#40FFFCC6", "color": TITLE_BLUE, "frame": 0, **g})
    w.append(label([30, 780, 160, 40], "Stamina", 3, CREAM_DIM, align="center", **g))
    w.append(label([24, 830, 168, 80], "", 6, STAMINA_GREEN, upright=True, align="center",
                   text_center_h=80, text_bind="totk.ready.stamina_text", **g))
    # Right: abilities.
    w += side_panel([1032, 232, 190, 490], accent_left=False)
    w.append(label([1040, 252, 170, 40], "Abilities", 3, CREAM_DIM, align="center", **g))
    w.append(label([1040, 304, 170, 400], "", 3, CREAM, align="center", wrap_width=170,
                   line_gap=12, text_bind="totk.ready.abilities_lines", **g))
    w += bottom_bar([("Inventory", "go_inventory"), ("Map", "go_map"), ("Settings", "go_options")])
    # Ready's buttons sit centred under the minimap, like BOTW's.
    for item in w[-6:]:
        item["rect"] = [item["rect"][0] + 228, *item["rect"][1:]]
    return {"id": "ready", "title": "Ready", "widgets": w + [unsupported_notice()]}


def map_page():
    g = {"need_bind": CONTENT_GATE}
    w = [rect([0, 0, W, H], "#FF000D17"),
         map_widget([0, 0, W, H], "world_map", pan_zoom=True, max_zoom=16,
                    marker_size=[72, 72], overlays=map_overlays("far", "near")),
         image([0, 0, W, 160], "file:assets/fade-down.png", tint="#C8000000", **g),
         label([0, 24, W, 72], "", 6, TITLE_BLUE, align="center", text_center_h=72,
               outline="#C8000A19", outline_px=3, text_bind="totk.map.layer_name", **g)]
    w += bottom_bar([("Back", "go_ready")], [("Layers", "map_layers")],
                    hint="Pinch to zoom - Drag to pan")
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
    w += bottom_bar([("Back", "go_ready")], [("Sort", "inventory_sort")])
    return {"id": "inventory", "title": "Inventory", "widgets": w + [unsupported_notice()]}


def options_page():
    w = [rect([0, 0, W, H], BG), rect([0, 0, W, 124], HEADER),
         label([40, 20, 800, 80], "Options", 6, CREAM, text_center_h=80),
         rect([24, 128, 1192, 2], RULE)]
    for i, name in enumerate(OPTIONS):
        y = 142 + i * 72
        w.append(rect([24, y, 1192, 64], TILE_BG, TILE_FRAME, frame=1, need_bind=CONTENT_GATE))
        w.append(label([56, y, 760, 64], name, 3, CREAM, text_center_h=64,
                       need_bind=CONTENT_GATE))
        w.append(label([800, y, 384, 64], "", 3, TITLE_BLUE, align="right", text_center_h=64,
                       text_bind=f"totk.option.{i}.text", need_bind=CONTENT_GATE))
    w += bottom_bar([("Back", "go_ready")])
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
        "map": {"areas": {"Hyrule": {"min": list(MAP_MIN), "max": list(MAP_MAX),
                                     "image": "file:assets/map-transparent.png",
                                     "clamp_view": True}},
                "style": {"background": "#FF000D17"}},
        "pages": [ready_page(), map_page(), inventory_page(), options_page()],
    }
    print(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
