#!/usr/bin/env python3
"""Generate packages/TearsOfTheKingdom/dualscreen/manifest.json.

The look is igawa6's Breath of the Wild companion, widget for widget where TOTK has the same
thing: the dark slate page with its soft top light, framed corner tiles for gear, round dials
around the minimap, see-through side panes with a blue edge, cream italic menu text and the
drawn command-bar buttons. The code-drawn frames and buttons come from that package
(dualscreen/assets, GPL-3.0, see ASSETS.provenance.json); game art and the game font come from
the player's romfs through the module.

    tools/totk/gen_manifest.py > packages/TearsOfTheKingdom/dualscreen/manifest.json
"""
import json

W, H = 1240, 1080
# BOTW companion palette.
BG = "#FF0C0F11"
BAND = "#E6141A1C"
LIFT = "#962A3436"
CREAM = "#FFFFFCC6"
CREAM_DIM = "#9EFFFCC6"
WHITE = "#FFFFFFFF"
BLUE = "#FF32C5FF"
HINT_BLUE = "#FF96D7F5"
ACCENT = "#FF0090FD"
RULE = "#50FFFCC6"
PANE = "#7D000000"
PANE_DARK = "#B4000000"
FRAME = "#46FFFCC6"
OUTLINE = "#C8000A19"
WARN = "#FFFFB74D"
LOW = "#FFEC4A34"
STAMINA_GREEN = "#FF6CE07A"
HEART_RED = "#FFE8434F"
ENERGY_BLUE = "#FF5FC8F0"
CLEAR = "#00000000"

# The game's System > Options list (amiibo, a toggle with no value, is left out).
OPTIONS = ["Invert Vertical Camera", "Invert Horizontal Camera", "Camera Sensitivity",
           "Aim with Motion Controls", "Invert Vertical Ability Controls",
           "Invert Horizontal Ability Controls", "Swap Jump Controls", "Lock Minimap to North",
           "HUD Mode", "Message Window", "Voice Language"]
# Optional cheats, all off until the player turns them on (module action cheat_toggle).
CHEATS = [("Infinite durability", "Equipped weapon, bow and shield never wear down"),
          ("Infinite arrows", "Your arrow count stays full"),
          ("Infinite rupees", "Your wallet stays at 999,999"),
          ("Unlimited battery", "Zonai battery stays charged"),
          ("Unlimited stamina", "The stamina wheel never drains")]
# Published by the module, which the host loads only on the supported build (module.build_ids).
CONTENT_GATE = "totk.supported"
G = {"need_bind": CONTENT_GATE}
# The world map: 12 x 10 cells of 1000 m, x east, map y = -z (north up). Cell i = row * 12 + col,
# row 0 the northern edge; the module publishes each cell's tile key for the layer on show.
MAP_MIN, MAP_MAX, CELL, COLS, ROWS = (-6000, -5000), (6000, 5000), 1000, 12, 10
# Link's marker; the module serves the arrow from the game's layout archive.
PLAYER_MARKER = "module:totk:player_marker"
# Shrine, lightroot and tower icons: white masks from the game's Common layout archive.
SHRINE_ICON = "module:totk:common/MapIconDungeon_L_Icon^s"
LIGHTROOT_ICON = "module:totk:common/MapIconArrivalPoint_L_FrameIcon^s"
TOWER_ICON = "module:totk:common/MapIconTower_00^s"


def asset(name):
    return f"file:assets/{name}.png"


def common(tex):
    """A texture of the game's Common layout archive, served by the module."""
    return f"module:totk:common/{tex}"


# ---- widget helpers (BOTW conventions: a label's rect is an anchor, align picks the side) ---

def rect(r, bg, color=CLEAR, **kw):
    return {"type": "rect", "rect": r, "bg": bg, "color": color, **kw}


def tap(r, action, **kw):
    return {"type": "rect", "rect": r, "color": CLEAR, "frame": 0, "on_tap": action, **kw}


def image(r, src, **kw):
    return {"type": "image", "rect": r, "src": src, **kw}


def text(x, y, s, scale=5, color=CREAM, align="center", upright=False, **kw):
    """Menu text, the game's leaning italic unless upright (numbers)."""
    style = ({"italic": False, "slant_deg": 0.0, "text_role": "numeral_readout"} if upright else
             {"italic": True, "slant_deg": 9.0, "text_role": "menu_text"})
    return {"type": "label", "rect": [x, y, 0, 0], "text": s, "text_scale": scale,
            "color": color, "align": align, **style, **kw}


def bound(x, y, bind, scale=5, color=CREAM, align="center", upright=False, **kw):
    return text(x, y, "", scale, color, align, upright, bind_text=bind, **kw)


def number(x, y, bind, scale=5, color=WHITE, align="center", **kw):
    return {"type": "value", "rect": [x, y, 0, 0], "bind": bind, "text_scale": scale,
            "color": color, "align": align, "outline": OUTLINE, "outline_px": 2,
            "italic": False, "slant_deg": 0.0, "text_role": "numeral_readout", **kw}


def page_base(band=False):
    """BOTW's page: slate, the soft light from the top, optionally the header band."""
    w = [rect([0, 0, W, H], BG),
         image([0, 0, W, 520], asset("page-lift"), tint=LIFT)]
    if band:
        w.append(rect([0, 0, W, 124], BAND))
    return w


# Command bar (cmdbar-<w>x112 is BOTW's drawn button; these widths ship).
BAR_Y, BAR_H = 944, 112


def command(x, w, label_text, action, color=CREAM, y=BAR_Y, **kw):
    return [image([x, y, w, BAR_H], asset(f"cmdbar-{w}x112-normal"), tint=WHITE, **kw),
            text(x + w // 2, y + 35, label_text, 6, color, fit_text=True, text_min_scale=4,
                 max_lines=1, wrap_width=w - 28, **kw),
            tap([x, y, w, BAR_H], action, **kw)]


def command_bound(x, w, bind, action, color=CREAM, y=BAR_Y, **kw):
    return [image([x, y, w, BAR_H], asset(f"cmdbar-{w}x112-normal"), tint=WHITE, **kw),
            bound(x + w // 2, y + 35, bind, 6, color, fit_text=True, text_min_scale=4,
                  max_lines=1, wrap_width=w - 28, **kw),
            tap([x, y, w, BAR_H], action, **kw)]


def footer_rule(x=24, w=1192, y=938):
    return rect([x, y, w, 1], RULE)


def pane(r, edge="left", bg=PANE, **kw):
    """BOTW's side pane: see-through black with a blue edge."""
    x, y, w, h = r
    ex = x if edge == "left" else x + w - 6
    return [rect(r, bg, **kw), rect([ex, y, 6, h], ACCENT, **kw)]


# A widget's hide_bind alone hides nothing: it hides outside keep_min..keep_max or at hide_eq.
# Every gate here is a 0/1 flag, so each one says hide_eq=1.


def unsupported_notice():
    """Two different failures: the module ran and saw another game build (it says which), or
    the module never loaded at all (nothing published)."""
    return [text(620, 420, "This companion needs Tears of the Kingdom version 1.4.2. Update the "
                 "game to use it.", 5, WARN, wrap_width=1120, need_bind="totk.unsupported"),
            bound(620, 580, "totk.build_text", 4, CREAM_DIM, need_bind="totk.unsupported"),
            text(620, 420, "The companion's native module did not start. Check that Eden Duo is "
                 "1.3.2.2 or newer, then reinstall the mod.", 5, WARN, wrap_width=1120,
                 hide_bind="totk.module", hide_eq=1),
            # The runtime's own reason when the module failed to load or stopped.
            bound(620, 640, "module_error_message", 4, CREAM_DIM, wrap_width=1120,
                  hide_bind="totk.module", hide_eq=1)]


# ---- map --------------------------------------------------------------------------------------

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


# Map markers, one slot per point on the layer on show (kind 0 = empty slot): the shrines,
# lightroots and towers you can travel to first, then the all-markers layer (koroks, bosses,
# caves and the rest) for the categories turned on, kept to the part of the map in view.
MARKER_SLOTS = 1200
TRAVEL_MARKERS = {"group": "travel", "count": MARKER_SLOTS, "x": "totk.travel.{i}.x",
                  "y": "totk.travel.{i}.y", "kind": "totk.travel.{i}.kind", "hide_when_kind": 0,
                  "icon_src_bind": "totk.travel.{i}.icon", "tint_bind": "totk.travel.{i}.tint",
                  "dim_bind": "totk.travel.{i}.dim", "size": 40, "selected_size": 60}


# The all-markers panel (Markers button): row 0 turns every category on or off, then one row per
# category in the module's order (totk_markers.h Category), then whether found ones stay shown.
MARKER_ROWS = ["All markers", "Shrines", "Lightroots", "Skyview towers", "Koroks", "Bosses",
               "Stables", "Towns", "Caves and wells", "Bubbul gems", "Poes", "Weapons",
               "Show found"]


def markers_panel():
    on = {"need_bind": "totk.markers.panel"}
    x, y0, w, row_h = 836, 132, 380, 60
    out = [rect([x, 120, w, 24 + row_h * len(MARKER_ROWS)], "#E6101416", FRAME, frame=1, **on),
           rect([x, 120, 6, 24 + row_h * len(MARKER_ROWS)], ACCENT, **on)]
    for i, name in enumerate(MARKER_ROWS):
        y = y0 + i * row_h
        if i in (1, len(MARKER_ROWS) - 1):
            out.append(rect([x + 24, y - 2, w - 48, 1], RULE, **on))
        out += [{"type": "image", "rect": [x + 22, y + 8, 40, 40],
                 "src_bind": f"totk.markers.{i}.icon", "tint_bind": f"totk.markers.{i}.tint", **on},
                text(x + 76, y + 12, name, 5, CREAM, align="left", fit_text=True,
                     text_min_scale=4, max_lines=1, wrap_width=220, **on),
                bound(x + w - 24, y + 12, f"totk.markers.{i}.text", 5, BLUE, align="right",
                      upright=True, **on),
                tap([x, y, w, row_h], f"markers_toggle_{i}", **on)]
    return out


def map_widget(r, wid, **kw):
    return {"type": "map", "id": wid, "area": "Hyrule", "rect": r, "area_label": False,
            "marker_x_bind": "totk.map.x", "marker_y_bind": "totk.map.y",
            "marker_src": PLAYER_MARKER, "need_bind": "totk.map.valid", **kw}


# ---- Ready --------------------------------------------------------------------------------------

MINIMAP = [220, 64, 800, 800]


def corner(x, y, slot, empty_text, low_layout=False):
    """A BOTW gear corner: the mask and frame, the item's icon and its durability number. The
    bottom corners put the number on top and the icon under it, like BOTW's weapon corner."""
    valid = f"totk.ready.{slot}.valid"
    w = [image([x, y, 184, 192], asset("ready-corner-mask"), tint=PANE),
         image([x, y, 184, 192], asset("ready-corner-frame"), tint=FRAME)]
    if low_layout:
        w += [bound(x + 92, y + 22, f"totk.ready.{slot}.durability_text", 5, WHITE, upright=True,
                    outline=OUTLINE, outline_px=2, need_bind=valid),
              {"type": "image", "rect": [x + 44, y + 67, 96, 96],
               "src_bind": f"totk.ready.{slot}.icon", "need_bind": valid}]
    else:
        w += [{"type": "image", "rect": [x + 50, y + 34, 84, 84],
               "src_bind": f"totk.ready.{slot}.icon", "need_bind": valid},
              bound(x + 92, y + 124, f"totk.ready.{slot}.durability_text", 5, WHITE,
                    upright=True, outline=OUTLINE, outline_px=2, need_bind=valid)]
    w += [text(x + 92, y + 82, empty_text, 4, CREAM_DIM, hide_bind=valid, hide_eq=1, **G),
          tap([x, y, 184, 192], "go_inventory")]
    return w


def dial(x, y, icon, bind, tint=CREAM):
    """One of BOTW's four round dials around the minimap, its count underneath."""
    return [image([x, y, 84, 84], asset("disc-84"), tint="#50FFFCC6", **G),
            image([x + 2, y + 2, 80, 80], asset("disc-80"), tint="#E6050607", **G),
            image([x + 14, y + 14, 56, 56], icon, tint=tint, **G),
            bound(x + 42, y + 90, bind, 5, WHITE, upright=True, outline=OUTLINE, outline_px=2,
                  **G)]


def ready_page():
    """BOTW's Ready page for TOTK: the round minimap ringed by stamina, gear in the corners,
    progress dials around the map, hearts and abilities in the side panes."""
    w = page_base()
    # Minimap: the square map widget, masked to a circle, lit from the top, framed.
    mx, my, mw, mh = MINIMAP
    w += [image(MINIMAP, asset("ready-circle-mask"), tint="#FF0A0D0F"),
          map_widget(MINIMAP, "ready_map", follow_window=1100, overlays=map_overlays("near"),
                     marker_size=[84, 84]),
          image(MINIMAP, asset("map-circle-outside"), tint=BG, need_bind="totk.map.valid"),
          image(MINIMAP, asset("pg-ready-lift-cover"), tint=LIFT, need_bind="totk.map.valid"),
          image([170, 14, 900, 900], asset("pg-ready-halo-mask"), tint="#B4000000"),
          image(MINIMAP, asset("ready-native-circle-frame"), tint="#6EFFFCC6"),
          {"type": "image", "rect": [mx - 11, my - 11, mw + 22, mh + 22],
           "bind": "totk.ready.stamina_frame",
           "src_names": [f"file:assets/stamina-{i:02d}.png" for i in range(51)],
           "tint": STAMINA_GREEN, **G},
          text(620, 432, "Minimap unavailable", 5, CREAM, hide_bind="totk.map.valid", hide_eq=1,
               **G),
          bound(620, 898, "totk.map.layer_name", 5, CREAM, **G)]
    # Dials: shrines and lightroots on top, towers and hearts below (BOTW's orbs, shrines,
    # sensor and temperature places).
    w += dial(258, 40, SHRINE_ICON, "totk.ready.shrines_text", "#FFF2A93B")
    w += dial(898, 40, LIGHTROOT_ICON, "totk.ready.lightroots_text", "#FF9BE36B")
    w += dial(258, 787, TOWER_ICON, "totk.ready.towers_text", "#FF5CC8F0")
    w += dial(898, 787, common("HeartW_00^s"), "totk.ready.hearts", HEART_RED)
    # Corners: shield and bow on top, Zonai battery and weapon at the bottom.
    w += corner(16, 16, "shield", "Shield")
    w += corner(1040, 16, "bow", "Bow")
    w += corner(1040, 872, "weapon", "Weapon", low_layout=True)
    w += [image([16, 872, 184, 192], asset("ready-corner-mask"), tint=PANE),
          image([16, 872, 184, 192], asset("ready-corner-frame"), tint=FRAME),
          image([70, 900, 76, 76], common("PouchIconRTEnergy_58x58^s"), tint=ENERGY_BLUE, **G),
          bound(108, 986, "totk.ready.battery_text", 5, WHITE, upright=True, outline=OUTLINE,
                outline_px=2, **G),
          {"type": "bar", "rect": [32, 1044, 152, 8], "bind": "totk.ready.battery_pct",
           "max": 100, "bg": "#40FFFCC6", "color": ENERGY_BLUE, "frame": 0, **G}]
    # Arrows: BOTW's teardrop under the bow.
    w += [image([1048, 206, 168, 188], asset("ready-native-arrow-mask"), tint=PANE),
          image([1048, 206, 168, 188], asset("ready-native-arrow-frame"), tint=FRAME),
          image([1080, 246, 100, 100], "module:totk:icon/Arrow", need_bind="totk.ready.arrows.valid"),
          number(1132, 352, "totk.ready.arrows", need_bind="totk.ready.arrows.valid"),
          tap([1048, 220, 168, 174], "go_inventory")]
    # Left pane: hearts and stamina (BOTW's quest pane).
    w += pane([16, 232, 184, 468], **G)
    w += [image([72, 252, 72, 72], common("HeartW_00^s"), tint=HEART_RED, **G),
          text(108, 330, "Hearts", 4, CREAM_DIM, **G),
          bound(108, 372, "totk.ready.hearts", 7, WHITE, upright=True, outline=OUTLINE,
                outline_px=2, **G),
          bound(108, 436, "totk.ready.hearts_of", 4, CREAM_DIM, **G),
          {"type": "bar", "rect": [36, 480, 144, 8], "bind": "totk.ready.life_pct", "max": 100,
           "bg": "#40FFFCC6", "color": HEART_RED, "frame": 0, **G},
          rect([36, 512, 144, 1], RULE, **G),
          text(108, 536, "Stamina", 4, CREAM_DIM, **G),
          bound(108, 580, "totk.ready.stamina_text", 7, STAMINA_GREEN, upright=True,
                outline=OUTLINE, outline_px=2, **G),
          text(108, 644, "wheels", 3, CREAM_DIM, **G)]
    # Rupees under the pane, where BOTW keeps them.
    rv = {"need_bind": "totk.ready.rupees.valid"}
    w += [text(108, 742, "Rupees", 4, CREAM_DIM, **rv),
          bound(108, 790, "totk.ready.rupees_text", 5, WHITE, fit_text=True, text_min_scale=4,
                max_lines=1, wrap_width=184, **rv)]
    # Right pane: abilities, two to a row (BOTW's food and buffs pane).
    w += pane([1032, 422, 192, 450], edge="right", **G)
    w += [text(1126, 432, "Abilities", 5, CREAM, **G),
          rect([1048, 478, 160, 1], RULE, **G),
          {"type": "image", "rect": [1052, 490, 72, 72], "src_bind": "totk.ready.ability.{i}.icon",
           "repeat": 8, "repeat_bind": "totk.ready.ability.count", "repeat_cols": 2,
           "repeat_dx": 82, "repeat_row_dy": 92, **G},
          text(1126, 640, "None yet", 4, CREAM_DIM, hide_bind="totk.ready.ability.count",
               keep_min=1, keep_max=99, **G)]
    # Command bar under the minimap.
    w.append(rect([216, 944, 808, 1], RULE))
    w += command(252, 228, "Inventory", "go_inventory", y=952)
    w += command(506, 228, "Map", "go_map", y=952)
    w += command(760, 228, "Settings", "go_options", y=952)
    return {"id": "ready", "title": "Ready", "widgets": w + unsupported_notice()}


def map_page():
    w = [rect([0, 0, W, H], "#FF000D17"),
         map_widget([0, 0, W, H], "world_map", pan_zoom=True, max_zoom=16,
                    marker_size=[72, 72], overlays=map_overlays("far", "near"),
                    on_marker_tap="travel_select", marker_tap_groups=["travel"], marker_hit_px=40,
                    empty_tap_deselects=True),
         # BOTW's blue glow along the top and bottom edges.
         image([0, 0, W, 200], asset("page-lift"), tint="#D2002850", **G),
         image([0, H - 200, W, 200], asset("page-lift"), tint="#D2002850", rotate=180, **G),
         bound(620, 30, "totk.map.layer_name", 6, BLUE, upright=True, outline=OUTLINE,
               outline_px=2, **G),
         text(620, 500, "Map unavailable here", 5, CREAM, hide_bind="totk.map.valid", hide_eq=1,
              **G)]
    # The tapped shrine, lightroot or tower: BOTW's popup panel.
    sel = {"need_bind": "totk.travel.sel.valid"}
    w += [rect([24, 752, 760, 176], "#F5101416", FRAME, frame=1, **sel),
          rect([24, 752, 6, 176], ACCENT, **sel),
          bound(56, 770, "totk.travel.sel.name", 6, CREAM, align="left", fit_text=True,
                text_min_scale=4, max_lines=1, wrap_width=700, **sel),
          bound(56, 834, "totk.travel.sel.status", 4, BLUE, align="left", **sel),
          bound(56, 878, "totk.travel.sel.hint", 4, CREAM_DIM, align="left", **sel)]
    w += command(24, 224, "Back", "go_ready", BLUE)
    w += markers_panel()
    w += [text(640, 988, "Pinch to zoom \u00b7 Drag to pan", 4, HINT_BLUE, wrap_width=270,
               max_lines=1, fit_text=True, text_min_scale=4, outline=OUTLINE, outline_px=2,
               hide_bind="totk.travel.sel.can_travel", hide_eq=1, **G)]
    w += command(264, 224, "Markers", "markers_panel", BLUE)
    w += command(784, 204, "Travel", "travel_go", BLUE, need_bind="totk.travel.sel.can_travel")
    w += command(1000, 216, "Layers", "map_layers", BLUE)
    return {"id": "map", "title": "Map", "widgets": w + unsupported_notice()}


# ---- Inventory ------------------------------------------------------------------------------------

# Inventory tabs: title, the game's tab icon (Common layout archive).
TABS = [("Weapons", "IconWeapon_72x72^s"), ("Bows and Arrows", "IconBow_72x72^s"),
        ("Shields", "IconShield_72x72^s"), ("Armor", "IconEquipment_72x72^s"),
        ("Materials", "IconMaterial_72x72^s"), ("Food", "IconCuisine_72x72^s"),
        ("Key Items", "IconTreasure_72x72^s")]
GRID_MAX = 420  # cells published per category at most
GRID = {"repeat": GRID_MAX, "repeat_bind": "totk.inv.count", "repeat_cols": 5, "repeat_dx": 148,
        "repeat_row_dy": 160, "scroll": "inv_grid"}


def inventory_page():
    """BOTW's Inventory: tab icons in the band (the title over the open one), a scrolling
    5-wide grid of the game's item icons, the selected item on the right."""
    w = page_base(band=True)
    w.append(footer_rule())
    for i, (title, icon) in enumerate(TABS):
        x = 40 + i * 112
        on = f"totk.inv.tab.{i}"
        w += [image([x, 36, 80, 80], common(icon), tint="#60FFFCC6", hide_bind=on, hide_eq=1, **G),
              image([x, 36, 80, 80], common(icon), tint=CREAM, need_bind=on),
              text(x + 40 if i else 24, 6, title, 5, CREAM, align="center" if i else "left",
                   need_bind=on),
              tap([x - 16, 8, 112, 112], f"inventory_category_{i}", **G)]
    w.append(rect([24, 128, 1192, 2], RULE))
    # Grid: ghost cell, equipped fill, icon, count, selection (light, blue frame, corners).
    w += [rect([24, 141, 140, 150], "#30000000", "#28FFFCC6", frame=1, **GRID, **G),
          rect([25, 142, 138, 148], ACCENT, need_bind="totk.inv.item.{i}.equipped", **GRID),
          {"type": "image", "rect": [48, 162, 92, 92], "src_bind": "totk.inv.item.{i}.icon",
           **GRID, **G},
          bound(151, 254, "totk.inv.item.{i}.qty", 4, WHITE, align="right", upright=True,
                outline=OUTLINE, outline_px=2, **GRID, **G),
          rect([27, 143, 134, 145], "#30FFFCC6", frame=0, need_bind="totk.inv.item.{i}.selected",
               **GRID),
          rect([26, 143, 136, 147], CLEAR, "#8C0090FD", frame=2,
               need_bind="totk.inv.item.{i}.selected", **GRID)]
    for dx, dy, rot in ((-9, 108, 0), (131, 108, 90), (131, 258, 180), (-9, 258, 270)):
        w.append(image([dx + 24, dy, 67, 67], asset("cell-corner"), tint=WHITE, rotate=rot,
                       pivot=[33.0, 33.0], need_bind="totk.inv.item.{i}.selected", **GRID))
    w += [tap([24, 141, 140, 150], "inventory_choose_{i}", **GRID, **G),
          bound(400, 500, "totk.inv.empty_text", 5, CREAM_DIM, **G)]
    # Detail pane.
    sel = {"need_bind": "totk.inv.sel.valid"}
    w += [rect([780, 140, 436, 796], PANE_DARK, **G),
          rect([788, 140, 6, 796], ACCENT, **G),
          {"type": "image", "rect": [874, 156, 240, 240], "src_bind": "totk.inv.sel.icon", **sel},
          bound(824, 410, "totk.inv.sel.name", 6, CREAM, align="left", wrap_width=384,
                max_lines=2, fit_text=True, text_min_scale=5, **sel),
          bound(824, 540, "totk.inv.sel.stat", 4, BLUE, align="left", **sel),
          rect([824, 614, 384, 2], RULE, **sel),
          bound(824, 638, "totk.inv.sel.desc", 4, CREAM, align="left", wrap_width=360,
                max_lines=0, line_gap=7, **sel)]
    w += command(24, 224, "Back", "go_ready")
    # The selected item's action (Equip / Unequip), when the module offers one.
    w += command_bound(264, 300, "totk.inv.sel.use_text", "inventory_use",
                       need_bind="totk.inv.sel.can_use")
    page = {"id": "inventory", "title": "Inventory", "widgets": w + unsupported_notice()}
    page["scrolls"] = [{"id": "inv_grid", "rect": [20, 136, 750, 796],
                        "count_bind": "totk.inv.count", "reset_bind": "totk.inv.category",
                        "row_h": 160, "cols": 5, "bar": "#B4FFFCC6", "bar_w": 5}]
    return page


# ---- Settings and Cheats ------------------------------------------------------------------------

def settings_rows(rows, y0=142, row_h=76):
    w = []
    for i, (name, value_bind, action, sub) in enumerate(rows):
        y = y0 + i * row_h
        w += [rect([24, y, 1192, row_h - 8], "#30000000", "#28FFFCC6", frame=1, **G),
              text(56, y + (8 if sub else 18), name, 5, CREAM, align="left", **G)]
        if sub:
            w.append(text(58, y + 42, sub, 3, CREAM_DIM, align="left", **G))
        w.append(bound(1184, y + 18, value_bind, 5, BLUE, align="right", **G))
        if action:
            w.append(tap([24, y, 1192, row_h - 8], action, **G))
    return w


def options_page():
    w = page_base(band=True)
    w += [text(40, 30, "Options", 6, CREAM, align="left"),
          rect([24, 128, 1192, 2], RULE), footer_rule()]
    w += settings_rows([(name, f"totk.option.{i}.text", None, None)
                        for i, name in enumerate(OPTIONS)], row_h=70)
    w += command(24, 224, "Back", "go_ready")
    w += command(992, 224, "Cheats", "go_cheats")
    return {"id": "options", "title": "Settings", "widgets": w + unsupported_notice()}


def cheats_page():
    w = page_base(band=True)
    w += [text(40, 30, "Cheats", 6, CREAM, align="left"),
          text(1200, 44, "Tap to turn on or off", 4, CREAM_DIM, align="right"),
          rect([24, 128, 1192, 2], RULE), footer_rule()]
    w += settings_rows([(name, f"totk.cheat.{i}.text", f"cheat_toggle_{i}", sub)
                        for i, (name, sub) in enumerate(CHEATS)], row_h=96)
    # Rows the module cannot do yet on this build say so instead of pretending.
    w.append(bound(620, 680, "totk.cheat.note", 4, CREAM_DIM, wrap_width=1100, **G))
    w += command(24, 224, "Back", "go_options")
    w += command(992, 224, "Add items", "go_spawn")
    return {"id": "cheats", "title": "Cheats", "widgets": w + unsupported_notice()}


SPAWN_TABS = [("Weapons", "IconWeapon_72x72^s"), ("Bows", "IconBow_72x72^s"),
              ("Shields", "IconShield_72x72^s"), ("Materials", "IconMaterial_72x72^s")]
SPAWN_GRID = {"repeat": GRID_MAX, "repeat_bind": "totk.spawn.count", "repeat_cols": 5,
              "repeat_dx": 148, "repeat_row_dy": 160, "scroll": "spawn_grid"}


def spawn_page():
    """Item spawning, laid out like the Inventory: tabs, the game's items in a grid, the chosen
    one on the right with Add buttons underneath."""
    w = page_base(band=True)
    w.append(footer_rule())
    for i, (title, icon) in enumerate(SPAWN_TABS):
        x = 40 + i * 112
        on = f"totk.spawn.tab.{i}"
        w += [image([x, 36, 80, 80], common(icon), tint="#60FFFCC6", hide_bind=on, hide_eq=1, **G),
              image([x, 36, 80, 80], common(icon), tint=CREAM, need_bind=on),
              text(x + 40 if i else 24, 6, title, 5, CREAM, align="center" if i else "left",
                   need_bind=on),
              tap([x - 16, 8, 112, 112], f"spawn_tab_{i}", **G)]
    w += [text(1200, 56, "Add items", 6, CREAM, align="right"),
          rect([24, 128, 1192, 2], RULE),
          rect([24, 141, 140, 150], "#30000000", "#28FFFCC6", frame=1, **SPAWN_GRID, **G),
          {"type": "image", "rect": [48, 162, 92, 92], "src_bind": "totk.spawn.item.{i}.icon",
           **SPAWN_GRID, **G},
          rect([27, 143, 134, 145], "#30FFFCC6", frame=0,
               need_bind="totk.spawn.item.{i}.selected", **SPAWN_GRID),
          rect([26, 143, 136, 147], CLEAR, "#8C0090FD", frame=2,
               need_bind="totk.spawn.item.{i}.selected", **SPAWN_GRID)]
    for dx, dy, rot in ((-9, 108, 0), (131, 108, 90), (131, 258, 180), (-9, 258, 270)):
        w.append(image([dx + 24, dy, 67, 67], asset("cell-corner"), tint=WHITE, rotate=rot,
                       pivot=[33.0, 33.0], need_bind="totk.spawn.item.{i}.selected",
                       **SPAWN_GRID))
    w += [tap([24, 141, 140, 150], "spawn_choose_{i}", **SPAWN_GRID, **G),
          bound(400, 500, "totk.spawn.empty_text", 5, CREAM_DIM, **G)]
    sel = {"need_bind": "totk.spawn.sel.valid"}
    w += [rect([780, 140, 436, 796], PANE_DARK, **G),
          rect([788, 140, 6, 796], ACCENT, **G),
          {"type": "image", "rect": [874, 156, 240, 240], "src_bind": "totk.spawn.sel.icon", **sel},
          bound(824, 410, "totk.spawn.sel.name", 6, CREAM, align="left", wrap_width=384,
                max_lines=2, fit_text=True, text_min_scale=5, **sel),
          rect([824, 540, 384, 2], RULE, **sel),
          bound(824, 560, "totk.spawn.sel.desc", 4, CREAM, align="left", wrap_width=360,
                max_lines=6, line_gap=7, **sel),
          bound(824, 800, "totk.spawn.result", 4, BLUE, align="left", wrap_width=370, **sel),
          text(824, 872, "Added items stay when the game saves.", 3, CREAM_DIM, align="left",
               wrap_width=370, **sel)]
    w += command(24, 224, "Back", "go_cheats")
    can = {"need_bind": "totk.spawn.can_add"}
    w += command(264, 224, "Add", "spawn_add_1", **can)
    w += command(504, 228, "Add 10", "spawn_add_10", need_bind="totk.spawn.sel.stack")
    page = {"id": "spawn", "title": "Add items", "widgets": w + unsupported_notice()}
    page["scrolls"] = [{"id": "spawn_grid", "rect": [20, 136, 750, 796],
                        "count_bind": "totk.spawn.count", "reset_bind": "totk.spawn.tab",
                        "row_h": 160, "cols": 5, "bar": "#B4FFFCC6", "bar_w": 5}]
    return page


NAV = ["ready", "map", "inventory", "options", "cheats", "spawn"]  # page ids


def main():
    actions = {f"go_{pid}": {"kind": "page", "page": pid, "transition": "fade", "duration_ms": 150}
               for pid in NAV}
    actions["travel_select"] = {"kind": "module", "action": "travel_select", "argument": "$payload"}
    for name in ["map_layers", "inventory_sort", "inventory_use", "travel_go", "markers_panel"]:
        actions[name] = {"kind": "module", "action": name, "argument": 0}
    for i in range(len(TABS)):
        actions[f"inventory_category_{i}"] = {"kind": "module", "action": "inventory_category",
                                              "argument": i}
    for i in range(GRID_MAX):
        actions[f"inventory_choose_{i}"] = {"kind": "module", "action": "inventory_choose",
                                            "argument": i}
    for i in range(len(SPAWN_TABS)):
        actions[f"spawn_tab_{i}"] = {"kind": "module", "action": "spawn_tab", "argument": i}
    for i in range(GRID_MAX):
        actions[f"spawn_choose_{i}"] = {"kind": "module", "action": "spawn_choose", "argument": i}
    for n in (1, 10):
        actions[f"spawn_add_{n}"] = {"kind": "module", "action": "spawn_add", "argument": n}
    for i in range(len(MARKER_ROWS)):
        actions[f"markers_toggle_{i}"] = {"kind": "module", "action": "markers_toggle", "argument": i}
    for i in range(len(CHEATS)):
        actions[f"cheat_toggle_{i}"] = {"kind": "module", "action": "cheat_toggle", "argument": i}
    manifest = {
        "format": 1,
        "title_id": "0100F2C0115B6000",
        "name": "Tears of the Kingdom companion",
        "min_runtime": 22,
        "_note": "Generated by tools/totk/gen_manifest.py. totk.supported comes from the module, "
                 "which only loads on 1.4.2; page values come from the module once located.",
        "requires_module": True,
        # Fast travel: a small guest helper (native/helper/totk/travel-hook.S) runs the game's
        # own warp request; tools/totk/build_helper_plan.py builds the plan.
        "load_plan": "load-plans/<build>.json",
        "load_plan_module": "main",
        "canvas_w": W,
        "canvas_h": H,
        "background": BG,
        # The game's menu font, rasterised by the module from the player's romfs.
        "font": "module:totk:font",
        "font_atlas": "module:totk:font-atlas",
        "actions": actions,
        "module_outputs": ["totk.module", "totk.supported", "totk.unsupported", "totk.ready"],
        "map": {"areas": {"Hyrule": {"min": list(MAP_MIN), "max": list(MAP_MAX),
                                     "image": "file:assets/map-transparent.png",
                                     "clamp_view": True,
                                     "dynamic_markers": [TRAVEL_MARKERS]}},
                "style": {"background": "#FF000D17"}},
        "pages": [ready_page(), map_page(), inventory_page(), options_page(), cheats_page(),
                  spawn_page()],
    }
    print(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
