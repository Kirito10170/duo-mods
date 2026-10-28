// SPDX-License-Identifier: GPL-3.0-or-later

// The Legend of Zelda: Tears of the Kingdom module: ABI glue.
// Supported build: 1.4.2 (update v655360), main 5CB42B1CF25469FB0635FD046453D843C18BC8AB.
// Every other build loads too, but only publishes the "update required" state (no reads).
//
// The Ready page (vitals, equipped gear, abilities), the world map, the inventory with equip and
// the game's options, read from its GameDataMgr (totk_gamedata.h); map tiles, icons and the font
// are decoded from the player's romfs (totk_assets.*, totk_font.*).
//
// Android is the target: nothing here may rely on guest calls (NCE has none), so changes to
// game state go through write_memory or button presses only.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <span>
#include <string>
#include <string_view>

#include "totk_assets.h"
#include "totk_font.h"
#include "totk_gamedata.h"
#include "totk_markers.h"
#include "totk_netlog.h"
#include "totk_travel.h"
#include "totk_travel_mailbox.h"

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100F2C0115B6000);
constexpr u8 BuildId[20]{0x5C, 0xB4, 0x2B, 0x1C, 0xF2, 0x54, 0x69, 0xFB, 0x06, 0x35,
                         0xFD, 0x04, 0x64, 0x53, 0xD8, 0x43, 0xC1, 0x8B, 0xC8, 0xAB};

EdenDsmodBool SupportsBuild(const char* build_id) {
    // Load on any build so an unsupported version can show its notice instead of a blank page.
    return build_id && std::strlen(build_id) >= 16 ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

// host->read_romfs as a whole-file reader (the base game plus update, as the game sees it).
TotkAssets::RomfsReader MakeRomfsReader(const EdenDsmodHostApi& h) {
    return [read_romfs = h.read_romfs,
            userdata = h.userdata](const std::string& path) -> std::optional<std::vector<u8>> {
        if (!read_romfs)
            return std::nullopt;
        const auto size = read_romfs(userdata, path.c_str(), 0, nullptr, 0);
        if (!size || size > 64 * 1024 * 1024)
            return std::nullopt;
        std::vector<u8> out(size);
        if (read_romfs(userdata, path.c_str(), 0, out.data(), size) != size)
            return std::nullopt;
        return out;
    };
}

using totk::gmd::Murmur3;

// ---- World map ------------------------------------------------------------------------------
// 12 x 10 cells of 1000 m; cell i = row * 12 + col, row 0 the northern edge (z = -5000).
constexpr int Cols = 12, Rows = 10, Cells = Cols * Rows;
enum Layer : int { Surface = 0, Sky = 1, Depths = 2 };
constexpr const char* LayerNames[]{"Surface", "Sky", "Depths"};
constexpr char LayerLetters[]{'G', 'S', 'U'};
// Number of T/F flag letters on each cell's tiles (0 = the cell has no tile), from the 1.4.2
// romfs. Depths tiles carry no flags. Each letter is one map region covering the cell.
constexpr std::string_view FlagCounts[2]{
    "112212221221111212222211113333122212120333433322232121230101112332420022121330233211222232"
    "233212101122122222111112122111",
    "112212221221110012222211110033122212120333433322232121230001112332020022121330033211222232"
    "003211101122002222111112122111",
};
static_assert(FlagCounts[0].size() == Cells && FlagCounts[1].size() == Cells);
// Zoom levels served to the pages: "near" (840 px per cell) for the minimap and close-up full
// map, "far" (135 px) for the zoomed-out full map.
constexpr const char* NearZoom = "01";
constexpr const char* FarZoom = "03";
// Provisional layer split by height until the game's own layer state is located.
// Layer of a position. No GameData flag says which layer Link is on, so: below the Depths cut is
// the Depths; above SkyAlways is the Sky; between SkyNear and SkyAlways it is the Sky only over a
// sky island (any actor of a Sky scene within a SkyGrid cell or its neighbours).
constexpr float DepthsBelow = -150.0f, SkyNear = 500.0f, SkyAlways = 1500.0f;
constexpr int SkyGrid = 100, SkyCols = 12000 / SkyGrid, SkyRows = 10000 / SkyGrid;

// Depths: the game keeps the Depths dark except around lit lightroots (computed, not a texture).
// The companion darkens each Depths tile outside LitRadius of every lit root; a tile's key carries
// a signature of the roots that touch it, so lighting one reloads only the tiles it reaches.
constexpr float LitRadius = 500.0f, LitFeather = 150.0f, DarkLevel = 0.28f;

std::string TileKey(int layer, int cell, const char* zoom, u32 lit_signature = 0) {
    char buf[64];
    const int col = cell % Cols, row = cell / Cols;
    int n = std::snprintf(buf, sizeof buf, "module:totk:map/%c_%02d-%02d_%s", LayerLetters[layer],
                          col, row, zoom);
    if (layer != Depths) {
        const int flags = FlagCounts[layer][static_cast<std::size_t>(cell)] - '0';
        if (flags == 0)
            return {};
        // Regions not yet revealed in game are not tracked yet: show every cell as revealed.
        buf[n++] = '_';
        for (int i = 0; i < flags; ++i)
            buf[n++] = 'T';
        buf[n] = 0;
    } else {
        std::snprintf(buf + n, sizeof buf - static_cast<std::size_t>(n), "~%08x", lit_signature);
    }
    return buf;
}

std::optional<std::size_t> SkyCell(float x, float z) {
    const int c = static_cast<int>(std::floor((x + 6000.0f) / SkyGrid));
    const int r = static_cast<int>(std::floor((z + 5000.0f) / SkyGrid));
    if (c < 0 || c >= SkyCols || r < 0 || r >= SkyRows)
        return std::nullopt;
    return static_cast<std::size_t>(r * SkyCols + c);
}

int LayerAt(float x, float y, float z, const std::vector<u8>& sky) {
    if (y < DepthsBelow)
        return Depths;
    if (y > SkyAlways)
        return Sky;
    if (y > SkyNear)
        if (const auto cell = SkyCell(x, z); cell && *cell < sky.size() && sky[*cell])
            return Sky;
    return Surface;
}

// ---- Travel points ----------------------------------------------------------------------------
// Read once from the player's scene files: Surface and Depths scenes are an A-J x 1-8 grid; the
// Sky scenes have one file per island group, so they are listed by name.
constexpr std::string_view SkyScenes[]{
    "Eldin_SkyIsland01", "Eldin_SkyIsland02", "Eldin_SkyIsland03", "Eldin_SkyIsland05",
    "Eldin_SkyIsland07", "Eldin_SkyIsland08_OutputAsSky", "Map_Fld_Zora_TrickArt_Set_A_01",
    "Map_SKy_Firone_SmallZonauRuins_Island_B_00", "Map_SKy_Firone_SmallZonauRuins_Island_D_00",
    "Pln_Fld_Rito_SkyField_Start_01", "Pln_Fld_Zora_SkyField_Before_DungeonWay",
    "SetEnemyBases_Drake_Mix_SkyIsland_PlacementSet_A-2",
    "SetEnemyBases_Drake_Mix_SkyIsland_PlacementSet_A-7",
    "SetEnemyBases_Drake_Mix_SkyIsland_PlacementSet_J-8", "Set_SkyIsland_Akkare_I-3_Sky",
    "Set_SkyIsland_Akkare_J-1_Sky", "Set_SkyIsland_Akkare_J-2_Sky", "Set_SkyIsland_Akkare_J-3_Sky",
    "Set_SkyIsland_CentralHyrule_D-6_Sky", "Set_SkyIsland_CentralHyrule_E-5_Sky_East",
    "Set_SkyIsland_CentralHyrule_E-5_Sky_West", "Set_SkyIsland_CentralHyrule_F-5_Sky_East",
    "Set_SkyIsland_CentralHyrule_F-5_Sky_West", "Set_SkyIsland_CentralHyrule_G-4_Sky",
    "Set_SkyIsland_Eldin_G-3_Sky", "Set_SkyIsland_Eldin_I-1_Sky",
    "Set_SkyIsland_Firone_F-7_Sky_Crystal", "Set_SkyIsland_Firone_F-7_Sky",
    "Set_SkyIsland_Firone_H-7_Sky_North", "Set_SkyIsland_Firone_H-7_Sky",
    "Set_SkyIsland_Gerudo_C-7_Sky", "Set_SkyIsland_Hateru_H-8_Sky", "Set_SkyIsland_Hebra_D-1_Sky",
    "Set_SkyIsland_HyruleForest_F-2_Sky", "Set_SkyIsland_HyruleForest_F-2_South",
    "Set_SkyIsland_HyruleHill_C-3_Sky", "Set_SkyIsland_HyruleHill_C-5_Sky",
    "Set_SkyIsland_HyruleLake_E-7_Sky", "Set_SkyIsland_Kakariko_G-6_Sky",
    "Set_SkyIsland_Lanayru_G-5_Sky", "Set_SkyIsland_Lanayru_I-6_Sky",
    "Set_SkyIsland_Lanayru_J-5_Sky", "Set_SkyIsland_Tabanta_B-2", "Set_SkyIsland_Tabanta_B-3_Sky",
    "Set_SkyIsland_Tabanta_B-4_Sky", "Set_SkyIsland_Zora_H-4_Sky", "Set_SkyIsland_Zora_H-5_Sky",
    "Set_SkyIsland_gerudo_A-6_Sky", "SkyIsland0069", "SkyIsland0070", "SkyIsland0071",
    "SkyIsland_Monument_01", "SkyIsland_Monument_02", "SkyIsland_Monument_03",
    "SkyIsland_Monument_04", "SkyIsland_Monument_06", "SkyIsland_Monument_07",
    "SkyIsland_Monument_08", "SkyIsland_Monument_09", "SkyIsland_Monument_10",
    "SkyIsland_Monument_11", "SkyIsland_Monument_12", "SkyIsland_Monument_13", "StartIslandMerge2",
};
// Shrines whose entrance sits in a cave, well or Hyrule Castle scene (Surface layer).
constexpr std::string_view ShrineScenes[]{
    "LargeDungeon/LargeDungeonHyruleCastle", "Castle/Set_HyruleCastle_IslandGround_A",
    "Cave/Cave_CentralHyrule_0009_GroupSet_000", "Cave/Cave_Eldin_0023_GroupSet_000",
    "Cave/Cave_Eldin_0031_GroupSet_000", "Cave/Cave_Eldin_0033_GroupSet_000",
    "Cave/Cave_Firone_0009_GroupSet_000", "Cave/Cave_Firone_0023_GroupSet_000",
    "Cave/Cave_FirstPlateau_0001_GroupSet_000", "Cave/Cave_GerudoDesert_0039_GruopSet_000",
    "Cave/Cave_GerudoDesert_0050_GroupSet_000", "Cave/Cave_HateruEast_0000_GroupSet_000",
    "Cave/Cave_HateruEast_0013_GroupSet_000", "Cave/Cave_Hebra_0013",
    "Cave/Cave_Hebra_0022_GroupSet_000", "Cave/Cave_Hebra_0037_GroupSet_000",
    "Cave/Cave_Hebra_0039_GroupSet_000", "Cave/Cave_HyruleForest_0008_GroupSet_000",
    "Cave/Cave_HyruleRidge_0003", "Cave/Cave_LanayruMountain_0004_GroupSet_000",
    "Cave/Cave_LanayruMountain_0008_GroupSet_000", "Cave/Cave_LanayruMountain_0010_GroupSet_000",
    "Cave/Cave_LanayruMountain_0022_GroupSet_000", "Cave/Cave_LanayruMountain_GroupSet_000",
    "Cave/Cave_Lanayru_0006_Group_Set_001", "Cave/Cave_Lanayru_0024_GroupSet_000",
    "Cave/Cave_Lanayru_0036_GroupSet_000", "Cave/Cave_Lanayru_0048_GroupSet_000",
    "Cave/Cave_Tabantha_0003_GroupSet_000", "Cave/Well_0006_GroupSet_000",
    "Cave/Well_0023_GroupSet_000",
};
// Map marker slots (manifest MARKER_SLOTS): travel points first, then the all-markers layer.
constexpr std::size_t MaxTravel = 1200;
// Marker pictures from the Common layout archive and their colours, by markers::Category (the
// first three are travel::Kind). The ^s icons are white masks; the ^w ones are drawn in colour.
constexpr const char* MarkerIcons[]{
    "module:totk:common/MapIconDungeon_L_Icon^s",   "module:totk:common/MapIconArrivalPoint_L_FrameIcon^s",
    "module:totk:common/MapIconTower_00^s",         "module:totk:common/MapIconKorok^w",
    "module:totk:common/MapIconSkull_72x72^w",      "module:totk:common/MapIconBuildingStableInn_58x58^w",
    "module:totk:common/MapIconBuildingVillage_58x58^w", "module:totk:common/MapIconCaveM_72x72^w",
    "module:totk:common/MapIconOre_72x72^w",        "module:totk:common/Nt_RupeeIconNew_00^w",
    "module:totk:common/MapIconWeapon_72x72^w"};
// Shrine amber, lightroot green, tower blue, korok leaf, boss red, stable tan, town cream, cave
// brown, Bubbul gem pink, poe violet, weapon steel.
constexpr s64 MarkerTints[]{0xFFF2A93B, 0xFF9BE36B, 0xFF5CC8F0, 0xFFCFE36A, 0xFFF06A5A, 0xFFF0D29A,
                            0xFFFFF4D6, 0xFFE0A86C, 0xFFF59AD8, 0xFFA99CFF, 0xFFD8DEE8};
static_assert(std::size(MarkerIcons) == totk::markers::NumCategories &&
              std::size(MarkerTints) == totk::markers::NumCategories);
// Canvas px per world unit (the whole map is about 0.1) before a dense category shows.
constexpr double MarkerMinPpw[]{0, 0, 0, 0.2, 0, 0, 0, 0, 0, 0.8, 0.4};

struct TravelPoint {
    totk::travel::Kind kind;
    int layer;
    float x, y, z;
    u32 state; // GameData Enum key hash (shrines, lightroots); 0 for towers
    std::string label; // "Dungeon021", "CheckPoint086", "Tower01" ("Tower" when unmatched)
};

struct World {
    std::vector<TravelPoint> points;
    std::vector<u8> sky; // SkyCols x SkyRows, 1 = sky island nearby
    std::vector<totk::markers::Marker> markers; // the all-markers layer
};

// Where a scene's markers sit: on a fixed layer, or by height like Link (the MainField grid
// holds the low sky islands too).
constexpr u8 LayerByHeight = 0xFF;

World LoadWorld(const TotkAssets::RomfsReader& read, const std::atomic<bool>& stop) {
    namespace mk = totk::markers;
    World world;
    world.sky.assign(static_cast<std::size_t>(SkyCols * SkyRows), 0);
    TotkAssets::Dictionaries dicts;
    if (const auto pack = read("/Pack/ZsDic.pack.zs"))
        dicts.Load(*pack);
    const auto file = [&](const std::string& path) -> std::optional<std::vector<u8>> {
        if (stop.load(std::memory_order_relaxed))
            return std::nullopt;
        const auto raw = read(path);
        return raw ? dicts.Decompress(*raw) : std::nullopt;
    };
    // Completion flags of koroks and bosses, from the GameData list the update ships.
    std::unordered_map<u32, mk::StructMembers> structs;
    for (const char* version : {"142", "141", "140", "130", "121", "120", "112", "110", "100"})
        if (const auto list = file(std::string{"/GameData/GameDataList.Product."} + version + ".byml.zs")) {
            constexpr u32 wanted[]{mk::KorokStruct, mk::CarryKorokStruct, mk::BossStruct};
            structs = mk::LoadStructs(*list, wanted);
            break;
        }
    const auto members = [&](u32 hash) -> const mk::StructMembers* {
        const auto it = structs.find(hash);
        return it == structs.end() ? nullptr : &it->second;
    };
    const auto* bosses = members(mk::BossStruct);
    std::unordered_map<std::string, bool> seen;
    std::unordered_map<u64, bool> seen_actors;
    std::vector<totk::travel::TowerTag> tags;
    // travel: look for shrines, lightroots and towers; markers: the layer's actors.
    const auto scene = [&](const std::string& path, int layer, bool travel, bool markers, u8 marker_layer) {
        const auto bytes = file(path);
        if (!bytes)
            return;
        if (travel)
            for (auto& p : totk::travel::Scan(*bytes, &tags)) {
                using totk::travel::Kind;
                std::string label = p.kind == Kind::Tower ? "Tower" : p.state_key.substr(p.state_key.find('.') + 1);
                // Shrine entrances and towers can be placed in more than one scene.
                char where[48];
                std::snprintf(where, sizeof where, "%s@%.0f,%.0f", label.c_str(), p.x, p.z);
                if (!seen.emplace(p.state_key.empty() ? where : p.state_key, true).second)
                    continue;
                world.points.push_back({p.kind, p.kind == Kind::Lightroot ? Depths : layer, p.x, p.y, p.z,
                                        p.state_key.empty() ? 0 : Murmur3(p.state_key), std::move(label)});
            }
        if (markers)
            totk::travel::ForEachActor(*bytes, [&](std::string_view gyaml, float x, float y, float z,
                                                   const totk::byml::Node& actor) {
                const auto cat = mk::ActorCategory(gyaml);
                if (!cat)
                    return;
                const u64 id = actor["Hash"].Integer().value_or(0);
                if (id && !seen_actors.emplace(id, true).second)
                    return;
                const u32 flag = *cat == mk::Category::Boss && bosses && id ? mk::MemberFlag(*bosses, id) : 0;
                world.markers.push_back({*cat, marker_layer, flag ? mk::Done::Bool : mk::Done::None, x, y, z,
                                         flag, std::string{gyaml}});
            });
        if (layer == Sky)
            totk::travel::ForEachActor(*bytes, [&](std::string_view, float x, float, float z, const auto&) {
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (const auto cell = SkyCell(x + dx * SkyGrid, z + dz * SkyGrid))
                            world.sky[*cell] = 1;
            });
    };
    for (const auto& [field, layer] : {std::pair{"MainField", Surface}, std::pair{"MinusField", Depths}})
        for (char col = 'A'; col <= 'J'; ++col)
            for (int row = 1; row <= 8; ++row) {
                const std::string base = std::string{"/Banc/"} + field + "/" + col + "-" + std::to_string(row);
                const u8 at = layer == Depths ? u8{Depths} : LayerByHeight;
                scene(base + "_Static.bcett.byml.zs", layer, true, true, at);
                scene(base + "_Dynamic.bcett.byml.zs", layer, false, true, at);
            }
    for (const auto name : SkyScenes)
        for (const char* part : {"_Static", "_Dynamic"})
            scene("/Banc/MainField/Sky/" + std::string{name} + part + ".bcett.byml.zs", Sky,
                  part[1] == 'S', true, Sky);
    // Shrine entrances in caves, wells and Hyrule Castle; the caves' own actors come next.
    for (const auto name : ShrineScenes)
        scene("/Banc/MainField/" + std::string{name} + "_Static.bcett.byml.zs", Surface, true,
              !name.starts_with("Cave/"), Surface);
    scene("/Banc/MainField/Castle/Set_HyruleCastle_IslandGround_A_Dynamic.bcett.byml.zs", Surface, false, true,
          Surface);
    for (const auto name : mk::CaveScenes)
        for (const char* part : {"_Static", "_Dynamic"})
            scene("/Banc/MainField/Cave/" + std::string{name} + part + ".bcett.byml.zs", Surface, false, true,
                  Surface);
    // Name each tower after the cannon beside it (within ~8 units in the 1.4.2 scenes).
    for (auto& p : world.points) {
        if (p.kind != totk::travel::Kind::Tower)
            continue;
        float best = 50.0f * 50.0f;
        for (const auto& t : tags)
            if (const float d = (t.x - p.x) * (t.x - p.x) + (t.z - p.z) * (t.z - p.z); d < best) {
                best = d;
                p.label = t.tower;
            }
    }
    // Places (stables, towns, caves, wells, chasms) and koroks have files of their own.
    if (const auto places = file("/Banc/MainField/LocationArea/MainField.locationarea.byml.zs"))
        mk::ScanPlaces(*places, Surface, world.markers);
    if (const auto places = file("/Banc/MinusField/LocationArea/MinusField.locationarea.byml.zs"))
        mk::ScanPlaces(*places, Depths, world.markers);
    if (const auto koroks = file("/Banc/MainField/HiddenKorok/MainField.hiddenkorok.byml.zs")) {
        const std::size_t first = world.markers.size();
        mk::ScanKoroks(*koroks, members(mk::KorokStruct), members(mk::CarryKorokStruct), world.markers);
        for (std::size_t i = first; i < world.markers.size(); ++i)
            world.markers[i].layer = LayerByHeight;
    }
    for (auto& m : world.markers)
        if (m.layer == LayerByHeight)
            m.layer = static_cast<u8>(LayerAt(m.x, m.y, m.z, world.sky));
    return world;
}

// ---- Ready page -----------------------------------------------------------------------------
struct AbilityName {
    std::string_view id, name, icon;
};
// Pouch.SpecialPower ids, display names and their icon actors in /UI/Tex/Icon.
constexpr AbilityName Abilities[]{
    {"UltraHand", "Ultrahand", "Obj_UltraHand"},
    {"Fuse", "Fuse", "Obj_OneTouchBond"},
    {"OneTouchBond", "Fuse", "Obj_OneTouchBond"},
    {"Ascend", "Ascend", "Obj_Tooreroof"},
    {"Tooreroof", "Ascend", "Obj_Tooreroof"},
    {"Recall", "Recall", "Obj_ReverseRecorder"},
    {"ReverseRecorder", "Recall", "Obj_ReverseRecorder"},
    {"Autobuild", "Autobuild", "Obj_AutoBuilder"},
    {"AutoBuilder", "Autobuild", "Obj_AutoBuilder"},
    {"Camera", "Camera", "Obj_Camera"},
};
constexpr std::size_t MaxAbilities = 8;

const AbilityName* FindAbility(std::string_view id) {
    if (id.starts_with("Obj_"))
        id.remove_prefix(4);
    for (const auto& a : Abilities)
        if (a.id == id)
            return &a;
    return nullptr;
}

std::string AbilityText(const std::vector<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) {
        if (id.empty())
            continue;
        std::string_view name = id;
        if (const auto* a = FindAbility(id))
            name = a->name;
        if (!out.empty())
            out += "\n";
        out += name;
    }
    return out;
}


// ---- Settings page --------------------------------------------------------------------------
// option.sav entries, in the game's System > Options order (amiibo left out). Hash 0 = not yet
// pinned down (two of the three candidate Bools D7459E39 / 83D9572E / 6C571FA3).
struct EnumValue {
    std::string_view id, text;
};
constexpr EnumValue Sensitivity[]{{"VerySlow", "Very Low"}, {"Slow", "Low"}, {"Normal", "Normal"},
                                  {"Fast", "High"},         {"VeryFast", "Very High"}};
constexpr EnumValue HudMode[]{{"Normal", "Normal"}, {"Pro", "Pro"}};
constexpr EnumValue MessageWindow[]{{"Normal", "Normal"}, {"Dark", "Dark"}};
struct OptionRow {
    u32 hash;
    std::span<const EnumValue> values; // empty = a Bool shown as On / Off
};
const OptionRow Options[]{
    {0x520DDC44, {}},       {0, {}},          {0x128D0120, Sensitivity}, {0xBB11D937, {}},
    {0, {}},                {0xD8A04EA6, {}}, {0xEF35E764, {}},          {0x25EEA466, {}},
    {0x4E767F10, HudMode}, {0xF22C2014, MessageWindow},
    {0, {}}, // voice language: Enum A2DCDC9D, value names not mapped yet
};

// ---- Inventory ----------------------------------------------------------------------------
// One tab per category, in the manifest's tab order; a tab may list more than one pouch (bows,
// then arrows). Gear shows durability (Content.Life; save editors' "unbreakable" values read as
// MAX), everything else its stack (Content.StockNum).
enum class Equip { None, Single, Armor };

struct Pouch {
    const char* name;
    bool gear;
    Equip equip;
};
// The category number the game's Equip and Remove take for a pouch.
std::optional<u32> PouchCategory(const Pouch& pouch) {
    const std::string_view name{pouch.name};
    return name == "Weapon" ? std::optional<u32>{0} : name == "Bow" ? std::optional<u32>{1}
         : name == "Shield" ? std::optional<u32>{3} : name == "Armor" ? std::optional<u32>{4}
         : std::nullopt;
}

// Food takes the menu's Eat (category 6) from the companion.
bool IsFood(const Pouch& pouch) {
    return std::string_view{pouch.name} == "Food";
}

struct Category {
    const char* title;
    std::array<Pouch, 2> pouches;
};
constexpr Category Categories[]{
    {"Weapons", {{{"Weapon", true, Equip::Single}}}},
    {"Bows and Arrows", {{{"Bow", true, Equip::Single}, {"Arrow", false, Equip::None}}}},
    {"Shields", {{{"Shield", true, Equip::Single}}}},
    {"Armor", {{{"Armor", false, Equip::Armor}}}},
    {"Materials", {{{"Material", false, Equip::None}}}},
    {"Food", {{{"Food", false, Equip::None}}}},
    {"Key Items", {{{"KeyItem", false, Equip::None}, {"SpecialParts", false, Equip::None}}}},
};
// Worn armor: element [0] of one IntArray per body part holds the Pouch.Armor slot (-1 = none).
// The key names are not in the game's list, only their hashes.
struct ArmorPart {
    std::string_view suffix;
    u32 hash;
};
constexpr ArmorPart ArmorParts[]{{"_Head", 0xC88D0A1E}, {"_Upper", 0x632017B5}, {"_Lower", 0x94CE9BBF}};
constexpr std::size_t MaxItems = 420;

// ---- Cheats -------------------------------------------------------------------------------
// The Cheats page rows, in the manifest's order. A row is offered once its value has been found
// and a write to it verified to stick.
enum CheatId : std::size_t { CheatDurability, CheatArrows, CheatRupees, CheatBattery, CheatStamina,
                             NumCheats };
constexpr bool CheatReady[NumCheats]{true, true, true, true, true};
// The player actor: the one heap object whose first word is this vtable. Its live stamina is
// the f32 at +0x1404 (GameData PlayerStatus.Stamina follows it).
constexpr u64 PlayerVtable = 0x3920648, PlayerStamina = 0x1404;
// Equipped gear keeps its live durability on the held actor as a pair of {u64 vtable, s32 value}
// entries (current, previous), 0x10 apart; the pouch's Content.Life follows them.
constexpr u64 DurabilityVtable = 0x391D670;
constexpr u64 DurabilityScanBytes = 32ull << 20; // heap scanned per sample while looking
constexpr s32 MaxArrows = 999, MaxRupees = 999'999;

// ---- Item spawning ------------------------------------------------------------------------
// The Spawn page's tabs: which pouch an item goes into and which actor names belong there. The
// catalog is every actor with both a name and a description in the message archive (items
// have both; enemies and objects have no description), filtered by prefix.
struct SpawnTab {
    const char* title;
    const char* pouch;
    u32 category; // the game's pouch category for its add-to-pouch call
    bool gear;
    std::array<std::string_view, 3> prefixes;
};
constexpr SpawnTab SpawnTabs[]{
    {"Weapons", "Weapon", 0, true, {"Weapon_Sword_", "Weapon_Lsword_", "Weapon_Spear_"}},
    {"Bows", "Bow", 1, true, {"Weapon_Bow_"}},
    {"Shields", "Shield", 3, true, {"Weapon_Shield_"}},
    {"Materials", "Material", 5, false, {"Item_", "Animal_Insect_"}},
};
// Cooked and roasted things go to the Food pouch with extra fields; they are not offered.
constexpr std::string_view SpawnSkip[]{"Item_Cook_", "Item_Roast_", "Item_Chilled_", "Item_Boiled_"};
constexpr s32 MaxStack = 999;
constexpr std::size_t MaxSpawnItems = 420;

std::string DurabilityText(s32 v) {
    if (v >= 2'000'000'000)
        return "MAX";
    return v >= 0 ? std::to_string(v) : std::string{};
}

// The game's menu font (Rodin bold), rasterised for labels at this cap height.
constexpr std::string_view BodyFont = "scft/ninP_RodinNTLG-B_DH_003_subset.bfotf";
constexpr std::uint32_t FontCap = 40;
constexpr std::uint8_t FontMarker[]{'T', 'O', 'T', 'K', 'F', 'O', 'N', 'T', '1'};

struct Module {
    bool supported{};
    u64 main_base{};
    TotkAssets::Decoder assets;
    // Built on the asset worker the first time the host asks for the font; read by decode_font
    // and the atlas image afterwards.
    std::mutex font_mutex;
    std::optional<TotkFont::Atlas> font;
    bool font_tried{};

    const TotkFont::Atlas* Font(const EdenDsmodHostApi* host) {
        std::scoped_lock lock{font_mutex};
        if (!font && !font_tried && host && host->read_romfs) {
            font_tried = true;
            font = TotkFont::LoadGameFont(MakeRomfsReader(*host), BodyFont, FontCap);
            if (font)
                TotkNetLog::Log("font: %zu glyphs, atlas %ux%u", font->glyphs.size(), font->image.width,
                                font->image.height);
            else
                TotkNetLog::Log("font: FAILED to build from %s", std::string{BodyFont}.c_str());
        }
        return font ? &*font : nullptr;
    }
    // Item display names, loaded once off the sample thread from the player's message archive.
    std::thread names_thread;
    std::atomic<bool> names_ready{false};
    std::unordered_map<std::string, TotkAssets::ActorText> names;

    // Travel points and the sky-island grid, scanned once off the sample thread.
    std::thread world_thread;
    std::atomic<bool> world_ready{false}, stopping{false};
    World world;

    ~Module() {
        stopping.store(true);
        if (names_thread.joinable())
            names_thread.join();
        if (world_thread.joinable())
            world_thread.join();
    }

    void StartWorld(const EdenDsmodHostApi& h) {
        if (world_thread.joinable() || !h.read_romfs)
            return;
        world_thread = std::thread([this, romfs = MakeRomfsReader(h)] {
            const auto t0 = std::chrono::steady_clock::now();
            try {
                world = LoadWorld(romfs, stopping);
            } catch (...) {
                world = {};
                TotkNetLog::Log("world: scan threw");
            }
            int kinds[3]{};
            for (const auto& p : world.points)
                ++kinds[static_cast<int>(p.kind)];
            std::size_t sky = 0;
            for (const u8 c : world.sky)
                sky += c;
            TotkNetLog::Log("world: %d shrines, %d lightroots, %d towers, %zu sky cells in %lld ms", kinds[0],
                            kinds[1], kinds[2], sky,
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now() - t0)
                                                       .count()));
            std::array<int, totk::markers::NumCategories> cats{};
            int flagged = 0;
            for (const auto& m : world.markers) {
                ++cats[static_cast<std::size_t>(m.cat)];
                flagged += m.done != totk::markers::Done::None;
            }
            TotkNetLog::Log("world: markers: %d koroks, %d bosses, %d stables, %d towns, %d caves, %d bubbulfrogs, "
                            "%d poes, %d weapons; %d with a done flag",
                            cats[3], cats[4], cats[5], cats[6], cats[7], cats[8], cats[9], cats[10], flagged);
            marker_done.assign(world.markers.size(), 0);
            world_ready.store(true, std::memory_order_release);
        });
    }

    void StartNames(const EdenDsmodHostApi& h) {
        if (names_thread.joinable() || !h.read_romfs)
            return;
        names_thread = std::thread([this, romfs = MakeRomfsReader(h)] {
            try {
                names = TotkAssets::LoadActorNames(romfs, "USen");
            } catch (...) {
                names.clear();
                TotkNetLog::Log("names: load threw");
            }
            TotkNetLog::Log("names: %zu entries", names.size());
            names_ready.store(true, std::memory_order_release);
        });
    }

    // Internal ids (Weapon_Sword_052) until the names are in.
    std::string ItemText(const std::string& id) const {
        if (names_ready.load(std::memory_order_acquire))
            if (const auto it = names.find(id); it != names.end() && !it->second.name.empty())
                return it->second.name;
        return id;
    }
    std::string ItemCaption(const std::string& id) const {
        if (names_ready.load(std::memory_order_acquire))
            if (const auto it = names.find(id); it != names.end())
                return it->second.caption;
        return {};
    }

    // ---- Inventory page state (guarded by `mutex`) ----
    int inv_category{0};
    int inv_selected{0};
    std::optional<std::pair<int, int>> inv_use; // {category, row} of a pressed Equip button
    std::array<bool, NumCheats> cheats{};       // the Cheats page toggles, all off at start
    // Infinite durability: the equipped items' live entries, found by a sliced heap scan.
    struct HeldGear {
        std::string id;              // the equipped item (re-scan when it changes)
        s32 target{-1};              // the durability to hold
        std::vector<u64> entries;    // guest addresses of matching {vtable, value} pairs
        bool searched{false};
        std::chrono::steady_clock::time_point searched_at{};
    };
    std::array<HeldGear, 3> held{};
    u64 scan_cursor{0};
    // Unlimited stamina: the player actor, found the same way.
    u64 player{0}, player_cursor{0};
    int spawn_tab{0}, spawn_selected{0};
    std::optional<std::pair<int, int>> spawn_request; // {tab, row} of a pressed Add button
    s32 spawn_amount{1};
    std::string spawn_result;                   // what the last Add did, for the page
    std::array<std::vector<std::string>, std::size(SpawnTabs)> spawn_catalog; // after names
    std::mutex mutex;  // guards the view state below (actions arrive off the sample thread)
    std::chrono::steady_clock::time_point last_heartbeat{};
    int view_layer{-1}; // -1 = follow Link
    std::vector<int> travel_slots; // map marker slot -> world.points index, or -1 - world.markers index
    int travel_selected{-1};       // world.points index
    int marker_selected{-1};       // world.markers index
    // The all-markers layer (Markers panel): categories on show, whether done ones stay.
    std::array<bool, totk::markers::NumCategories> marker_on{};
    bool markers_panel{false}, show_found{true};
    std::vector<u8> marker_done; // sample thread: 1 = found / defeated, refreshed a slice at a time
    std::size_t done_cursor{0};
    int link_layer{Surface};

    void Configure(const EdenDsmodHostExtensions* ext) {
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext)) {
            assets.SetAstcDecoder({ext->userdata, ext->decode_astc});
            mailbox = ext;
        }
    }

    // ---- Travel: the guest helper's mailbox (native/helper/totk/travel-hook.S) ----
    const EdenDsmodHostExtensions* mailbox{};
    u32 mailbox_heartbeat{0}, travel_seq{0}, pending_op{0};
    bool helper_alive{false}, helper_busy{false};
    std::chrono::steady_clock::time_point helper_seen{}, travel_sent{}, travel_landed{};
    std::string travel_message;   // what the last Travel did, for the map panel
    std::string equip_message;    // what the last Equip or Eat did, for the inventory panel
    std::string pending_label;    // the item an Add is for, for its result line
    int travel_request{-1};       // world.points index the Travel button asked for

    bool Load32(u32 offset, u32& v) const {
        return mailbox && mailbox->load_u32 && mailbox->load_u32(mailbox->userdata, offset, &v) == EDEN_DSMOD_TRUE;
    }
    bool Store32(u32 offset, u32 v) const {
        return mailbox && mailbox->store_u32 && mailbox->store_u32(mailbox->userdata, offset, v) == EDEN_DSMOD_TRUE;
    }

    // Once per sample: is the helper ticking, keep the epoch in step, collect an answer.
    void PollHelper(const EdenDsmodHostApi& h) {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        u32 beat{};
        if (Load32(totk::mailbox::Heartbeat, beat) && beat != mailbox_heartbeat) {
            mailbox_heartbeat = beat;
            helper_seen = now;
        }
        helper_alive = helper_seen.time_since_epoch().count() != 0 && now - helper_seen < seconds(2);
        if (const s64 host_epoch = h.get_i64 ? h.get_i64(h.userdata, "__mailbox_epoch", 0) : 0; host_epoch > 0)
            Store32(totk::mailbox::Epoch, static_cast<u32>(host_epoch));
        else if (u32 e{}; Load32(totk::mailbox::Epoch, e) && e == 0)
            Store32(totk::mailbox::Epoch, 1);
        if (!helper_busy)
            return;
        u32 done{}, code{};
        if (Load32(totk::mailbox::ResultSeq, done) && done == travel_seq && Load32(totk::mailbox::ResultCode, code)) {
            helper_busy = false;
            TotkNetLog::Log("helper: op %u answered %u", pending_op, code);
            if (pending_op == totk::mailbox::OpEat)
                LogEatResult(h);
            if (pending_op == totk::mailbox::OpAdd) {
                std::scoped_lock lock{mutex};
                spawn_result = code == 0 ? "Added " + pending_label
                             : code == 1 ? "The game couldn't add that (pouch full?)"
                                         : "Adding items isn't available right now";
                return;
            }
            if (pending_op != totk::mailbox::OpTravel) {
                equip_message = code == 0 ? "" : code == 1 ? "The game didn't allow that right now"
                                                           : "That isn't available right now";
                return;
            }
            if (code == 0)
                travel_landed = now; // the game is loading; no second request until it's done
            travel_message = code == 0 ? "" : code == 1 ? "The game refused this destination"
                                                        : "Travel isn't available right now";
        } else if (now - travel_sent > seconds(5)) {
            helper_busy = false;
            if (pending_op == totk::mailbox::OpAdd) {
                std::scoped_lock lock{mutex};
                spawn_result = "The game didn't answer; try again";
            } else {
                (pending_op == totk::mailbox::OpTravel ? travel_message : equip_message) =
                    "The game didn't answer; try again";
            }
        }
    }

    // Debug log only: the pouch's last-eat fields (heal accepted byte at +0x364, then the heal
    // and effect values the game sent), to tell "game refused" from "never asked" on a device.
    void LogEatResult(const EdenDsmodHostApi& h) {
        u64 pouch{};
        if (!h.read_memory || h.read_memory(h.userdata, h.main_base + 0x3A6F678, &pouch, 8) != EDEN_DSMOD_TRUE ||
            !pouch)
            return;
        std::array<u8, 0x60> f{};
        if (h.read_memory(h.userdata, pouch + 0x360, f.data(), f.size()) != EDEN_DSMOD_TRUE)
            return;
        u32 w368, w3b4;
        u64 q370, q378;
        std::memcpy(&w368, f.data() + 0x08, 4);
        std::memcpy(&q370, f.data() + 0x10, 8);
        std::memcpy(&q378, f.data() + 0x18, 8);
        std::memcpy(&w3b4, f.data() + 0x54, 4);
        TotkNetLog::Log("eat: accepted %u, +0x368 %08x, +0x370 %016llx, +0x378 %016llx, +0x3B4 %08x",
                        f[0x04], w368, static_cast<unsigned long long>(q370),
                        static_cast<unsigned long long>(q378), w3b4);
    }

    // Hand one request to the helper: name (if any) and arguments first, the sequence word last.
    bool SendRequest(u32 op, u32 arg0, u32 arg1, std::string_view name) {
        if (!helper_alive || helper_busy || name.size() >= totk::mailbox::NameSize)
            return false;
        u32 epoch{}, seq{};
        if (!Load32(totk::mailbox::Epoch, epoch) || epoch == 0 || !Load32(totk::mailbox::ResultSeq, seq))
            return false;
        bool ok = true;
        if (!name.empty()) {
            std::array<u8, totk::mailbox::NameSize> text{};
            std::memcpy(text.data(), name.data(), name.size());
            for (u32 i = 0; i < text.size(); i += 4) {
                u32 word;
                std::memcpy(&word, text.data() + i, 4);
                ok = Store32(totk::mailbox::Name + i, word) && ok;
            }
        }
        ok = Store32(totk::mailbox::Arg0, arg0) && ok;
        ok = Store32(totk::mailbox::Arg1, arg1) && ok;
        ok = Store32(totk::mailbox::ReqOp, op) && ok;
        ok = Store32(totk::mailbox::ReqEpoch, epoch) && ok;
        travel_seq = seq + 1;
        ok = ok && Store32(totk::mailbox::ReqSeq, travel_seq); // last: hands the request over
        TotkNetLog::Log("helper: op %u (%u, %u) %.*s %s", op, arg0, arg1, static_cast<int>(name.size()),
                        name.data(), ok ? "staged" : "store FAILED");
        if (ok) {
            helper_busy = true;
            pending_op = op;
            travel_sent = std::chrono::steady_clock::now();
        }
        return ok;
    }

    // Stage "StartPos_<name>" for the helper to hand to the game's warp request.
    bool SendTravel(const TravelPoint& p) {
        using totk::travel::Kind;
        const std::string name = std::string{p.kind == Kind::Shrine ? "StartPos_C" : "StartPos_"} + p.label;
        const bool ok = SendRequest(totk::mailbox::OpTravel, 0, 0, name);
        if (ok)
            travel_message = "Traveling...";
        return ok;
    }

    // Asset worker thread: touches only `assets` (internally locked) and the borrowed host.
    EdenDsmodBool LoadImage(const EdenDsmodHostApi* host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        constexpr std::string_view prefix{"module:totk:"};
        if (!host || !host->read_romfs || !key || !sink)
            return EDEN_DSMOD_FALSE;
        const std::string_view k{key};
        if (!k.starts_with(prefix))
            return EDEN_DSMOD_FALSE;
        const auto logical = k.substr(prefix.size());
        if (logical == "font-atlas") {
            const auto* atlas = Font(host);
            if (!atlas)
                return EDEN_DSMOD_FALSE;
            sink(receiver, atlas->image.width, atlas->image.height, atlas->image.rgba.data(),
                 atlas->image.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        const auto romfs = MakeRomfsReader(*host);
        std::shared_ptr<const TotkAssets::Image> image;
        // "common/<tex>": a texture of the Common layout archive, whose versioned name the
        // update may change; "player_marker" is Link's arrow from it.
        std::string common;
        if (logical == "player_marker")
            common = "Nt_MapPlayerIconNew_00^s";
        else if (logical.starts_with("common/"))
            common = logical.substr(7);
        if (!common.empty()) {
            for (const char* archive : {"Common.Product.140.Nin_NX_NVN", "Common.Product.142.Nin_NX_NVN",
                                        "Common.Product.120.Nin_NX_NVN", "Common.Product.110.Nin_NX_NVN",
                                        "Common.Product.100.Nin_NX_NVN"})
                if ((image = assets.Load(romfs, std::string{"lyt/"} + archive + "/" + common)))
                    break;
        } else if (logical.starts_with("map/U_") && logical.find('~') != std::string_view::npos) {
            image = LitDepthsTile(romfs, logical);
        } else {
            image = assets.Load(romfs, logical);
            // A map tile exists only for the flag combinations the game ships; the module asks for
            // every region revealed (all T). When that one is missing, take the combination with
            // the most T letters that does exist.
            const auto flags_at = logical.rfind('_');
            if (!image && logical.starts_with("map/") && flags_at != std::string_view::npos &&
                flags_at + 1 < logical.size() &&
                logical.find_first_not_of("TF", flags_at + 1) == std::string_view::npos) {
                // The per-cell flag counts may be off too, so other lengths come after.
                const std::string base{logical.substr(0, flags_at + 1)};
                const std::size_t want = logical.size() - flags_at - 1;
                std::vector<std::size_t> lengths{want};
                for (std::size_t n = 1; n <= 4; ++n)
                    if (n != want)
                        lengths.push_back(n);
                for (const std::size_t n : lengths) {
                    std::vector<unsigned> masks(1u << n);
                    for (unsigned m = 0; m < masks.size(); ++m)
                        masks[m] = m;
                    std::stable_sort(masks.begin(), masks.end(), [](unsigned a, unsigned b) {
                        return std::popcount(a) > std::popcount(b);
                    });
                    for (const unsigned m : masks) {
                        std::string key = base;
                        for (std::size_t i = 0; i < n; ++i)
                            key += (m >> (n - 1 - i)) & 1 ? 'T' : 'F';
                        if (key != logical && (image = assets.Load(romfs, key))) {
                            TotkNetLog::Log("tile %s -> %s", std::string{logical}.c_str(), key.c_str());
                            break;
                        }
                    }
                    if (image)
                        break;
                }
            }
        }
        if (!image) {
            TotkNetLog::Log("image FAILED: %s", key);
            return EDEN_DSMOD_FALSE;
        }
        sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    EdenDsmodBool OnAction(std::string_view action, std::int64_t arg) {
        TotkNetLog::Log("action %.*s %lld", static_cast<int>(action.size()), action.data(),
                        static_cast<long long>(arg));
        if (action == "inventory_category" && arg >= 0 &&
            arg < static_cast<std::int64_t>(std::size(Categories))) {
            std::scoped_lock lock{mutex};
            if (inv_category != arg)
                inv_selected = 0;
            inv_category = static_cast<int>(arg);
            return EDEN_DSMOD_TRUE;
        }
        if (action == "inventory_choose" && arg >= 0 && arg < static_cast<std::int64_t>(MaxItems)) {
            std::scoped_lock lock{mutex};
            inv_selected = static_cast<int>(arg);
            return EDEN_DSMOD_TRUE;
        }
        if (action == "inventory_use") {
            std::scoped_lock lock{mutex};
            inv_use = std::pair{inv_category, inv_selected};
            return EDEN_DSMOD_TRUE;
        }
        if (action == "travel_select") {
            std::scoped_lock lock{mutex};
            const bool known = arg >= 0 && static_cast<std::size_t>(arg) < travel_slots.size();
            const int slot = known ? travel_slots[static_cast<std::size_t>(arg)] : -1;
            const int point = known && slot >= 0 ? slot : -1, marker = known && slot < 0 ? -1 - slot : -1;
            travel_selected = travel_selected == point ? -1 : point;
            marker_selected = marker_selected == marker ? -1 : marker;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "markers_panel") {
            std::scoped_lock lock{mutex};
            markers_panel = !markers_panel;
            return EDEN_DSMOD_TRUE;
        }
        // Row 0: every category; then one per category; the last row: show found ones.
        if (action == "markers_toggle" && arg >= 0 &&
            arg <= static_cast<std::int64_t>(totk::markers::NumCategories) + 1) {
            std::scoped_lock lock{mutex};
            const auto i = static_cast<std::size_t>(arg);
            if (i == 0) {
                const bool all = std::ranges::all_of(marker_on, [](bool on) { return on; });
                marker_on.fill(!all);
            } else if (i <= marker_on.size()) {
                marker_on[i - 1] = !marker_on[i - 1];
            } else {
                show_found = !show_found;
            }
            return EDEN_DSMOD_TRUE;
        }
        if (action == "cheat_toggle" && arg >= 0 && arg < static_cast<std::int64_t>(NumCheats)) {
            if (!CheatReady[arg])
                return EDEN_DSMOD_FALSE;
            std::scoped_lock lock{mutex};
            cheats[static_cast<std::size_t>(arg)] = !cheats[static_cast<std::size_t>(arg)];
            TotkNetLog::Log("cheat %lld %s", static_cast<long long>(arg),
                            cheats[static_cast<std::size_t>(arg)] ? "on" : "off");
            return EDEN_DSMOD_TRUE;
        }
        if (action == "spawn_tab" && arg >= 0 && arg < static_cast<std::int64_t>(std::size(SpawnTabs))) {
            std::scoped_lock lock{mutex};
            if (spawn_tab != arg)
                spawn_selected = 0;
            spawn_tab = static_cast<int>(arg);
            spawn_result.clear();
            return EDEN_DSMOD_TRUE;
        }
        if (action == "spawn_choose" && arg >= 0 && arg < static_cast<std::int64_t>(MaxSpawnItems)) {
            std::scoped_lock lock{mutex};
            spawn_selected = static_cast<int>(arg);
            spawn_result.clear();
            return EDEN_DSMOD_TRUE;
        }
        if (action == "spawn_add" && arg >= 1 && arg <= MaxStack) {
            std::scoped_lock lock{mutex};
            spawn_request = std::pair{spawn_tab, spawn_selected};
            spawn_amount = static_cast<s32>(arg);
            return EDEN_DSMOD_TRUE;
        }
        if (action == "travel_go") {
            std::scoped_lock lock{mutex};
            if (travel_selected < 0)
                return EDEN_DSMOD_FALSE;
            travel_request = travel_selected;
            return EDEN_DSMOD_TRUE;
        }
        if (action == "map_layers") {
            std::scoped_lock lock{mutex};
            const int from = view_layer < 0 ? link_layer : view_layer;
            view_layer = (from + 1) % 3;
            return EDEN_DSMOD_TRUE;
        }
        return EDEN_DSMOD_FALSE;
    }

    void Sample(const EdenDsmodHostApi& h) {
        auto read = [&h](u64 at, void* out, std::size_t size) {
            return h.read_memory && h.read_memory(h.userdata, at, out, size) == EDEN_DSMOD_TRUE;
        };
        StartNames(h);
        StartWorld(h);
        PollHelper(h);
        totk::gmd::Reader gmd{read, h.main_base};
        const bool live = gmd.Begin();
        h.publish_i64(h.userdata, "totk.ready", live ? 1 : 0);
        if (const auto now = std::chrono::steady_clock::now(); now - last_heartbeat > std::chrono::seconds(10)) {
            last_heartbeat = now;
            const auto pos = live ? gmd.GetVec3(Murmur3("PlayerStatus.CurrentPos")) : std::nullopt;
            TotkNetLog::Log("sample: gamedata %s, pos %.0f %.0f %.0f, layer %d, world %d, names %d",
                            live ? "live" : "not built", pos ? pos->x : 0.0f, pos ? pos->y : 0.0f,
                            pos ? pos->z : 0.0f, link_layer, world_ready.load() ? 1 : 0,
                            names_ready.load() ? 1 : 0);
        }
        if (!live) {
            h.publish_i64(h.userdata, "totk.map.valid", 0);
            return;
        }
        PublishVitals(h, gmd);
        PublishGear(h, gmd);
        UpdateLitRoots(gmd);
        PublishMap(h, gmd);
        PublishTravel(h, gmd);
        PublishProgress(h, gmd);
        PublishOptions(h, gmd);
        PublishInventory(h, gmd);
        ApplyCheats(h, gmd);
        PublishSpawn(h, gmd);
    }

    // Unlimited stamina: keep the player's live stamina at the maximum.
    template <class G>
    void HoldStamina(const EdenDsmodHostApi& h, G& gmd) {
        const u64 vtable = h.main_base + PlayerVtable;
        if (player) {
            u64 vt{};
            if (!h.read_memory(h.userdata, player, &vt, 8) || vt != vtable)
                player = 0; // a load rebuilt the player
        }
        if (!player) {
            player = ScanFor(h, player_cursor, [&](const u8* p, u64) {
                u64 vt;
                std::memcpy(&vt, p, 8);
                return vt == vtable;
            });
            if (player)
                TotkNetLog::Log("stamina: player at %llx", static_cast<unsigned long long>(player));
            return;
        }
        const auto max = gmd.GetFloat(Murmur3("PlayerStatus.MaxStamina"));
        float now{};
        if (!max || !std::isfinite(*max) || *max <= 0 ||
            !h.read_memory(h.userdata, player + PlayerStamina, &now, 4) || !(now < *max))
            return;
        const float full = *max;
        h.write_memory(h.userdata, player + PlayerStamina, &full, sizeof full);
    }

    // One slice of a heap scan at 8-byte steps from `cursor`; the address of the first match,
    // or 0 (the cursor moves on, wrapping at the end of the heap).
    template <class Match>
    u64 ScanFor(const EdenDsmodHostApi& h, u64& cursor, Match match) {
        if (!h.get_heap_begin || !h.get_heap_end)
            return 0;
        const u64 begin = h.get_heap_begin(h.userdata), end = h.get_heap_end(h.userdata);
        if (end <= begin)
            return 0;
        if (cursor < begin || cursor >= end)
            cursor = begin;
        const u64 stop = std::min(end, cursor + DurabilityScanBytes);
        std::vector<u8> chunk((1u << 20) + 0x20);
        for (u64 at = cursor; at < stop; at += 1u << 20) {
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(chunk.size(), end - at));
            if (n < 8 || !h.read_memory(h.userdata, at, chunk.data(), n))
                continue;
            for (std::size_t o = 0; o + 8 <= n && o < (1u << 20); o += 8)
                if (match(chunk.data() + o, at + o)) {
                    cursor = at + o + 8;
                    return at + o;
                }
        }
        cursor = stop >= end ? begin : stop;
        return 0;
    }

    // Infinite durability for the equipped weapon, bow and shield. The pouch value is copied
    // from the held actor every frame, so the actor's own entry is what gets held: found once
    // per equip by scanning the heap for the durability pair with the pouch's value, then
    // rewritten whenever it drops.
    template <class G>
    void HoldDurability(const EdenDsmodHostApi& h, G& gmd) {
        constexpr const char* Pouches[]{"Weapon", "Bow", "Shield"};
        const u64 vtable = h.main_base + DurabilityVtable;
        bool need_scan = false;
        std::array<s32, 3> life{};
        for (std::size_t k = 0; k < 3; ++k) {
            auto& g = held[k];
            const std::string base = std::string{"Pouch."} + Pouches[k];
            const auto names = gmd.GetString64Array(Murmur3(base + ".Content.Name"));
            const auto lives = gmd.GetIntArray(Murmur3(base + ".Content.Life"));
            const auto equip = gmd.GetIntArray(Murmur3(base + ".EquipIndex"));
            const int index = equip && !equip->empty() ? (*equip)[0] : -1;
            const bool valid = names && lives && index >= 0 &&
                               static_cast<std::size_t>(index) < std::min(names->size(), lives->size()) &&
                               !(*names)[static_cast<std::size_t>(index)].empty();
            const std::string id = valid ? (*names)[static_cast<std::size_t>(index)] : std::string{};
            if (id != g.id) {
                g = {};
                g.id = id;
            }
            if (!valid)
                continue;
            life[k] = (*lives)[static_cast<std::size_t>(index)];
            if (g.target < 0)
                g.target = life[k]; // what it had when the cheat (or this item) came on
            // Keep the found entries topped up; drop any that stopped looking like ours (a load
            // rebuilds the actor), and look again once none are left.
            const bool had = !g.entries.empty();
            std::erase_if(g.entries, [&](u64 at) {
                u64 vt{};
                s32 v{};
                if (!h.read_memory(h.userdata, at, &vt, 8) || vt != vtable ||
                    !h.read_memory(h.userdata, at + 8, &v, 4))
                    return true;
                if (v < g.target)
                    h.write_memory(h.userdata, at + 8, &g.target, sizeof g.target);
                return false;
            });
            if (had && g.entries.empty())
                g.searched = false;
            // Not found on a full pass (still loading, say): try again a little later.
            if (g.searched && g.entries.empty() &&
                std::chrono::steady_clock::now() - g.searched_at > std::chrono::seconds(15))
                g.searched = false;
            if (g.entries.empty() && !g.searched)
                need_scan = true;
        }
        if (!need_scan || !h.get_heap_begin || !h.get_heap_end)
            return;
        const u64 begin = h.get_heap_begin(h.userdata), end = h.get_heap_end(h.userdata);
        if (end <= begin)
            return;
        if (scan_cursor < begin || scan_cursor >= end)
            scan_cursor = begin;
        const u64 stop = std::min(end, scan_cursor + DurabilityScanBytes);
        std::vector<u8> chunk(1u << 20);
        for (u64 at = scan_cursor; at < stop; at += chunk.size()) {
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(chunk.size() + 0x20, end - at));
            chunk.resize(std::max<std::size_t>(n, 0x20));
            if (n < 0x20 || !h.read_memory(h.userdata, at, chunk.data(), n)) {
                chunk.resize(1u << 20);
                continue;
            }
            for (std::size_t o = 0; o + 0x1C <= n && o < (1u << 20); o += 8) {
                u64 vt0, vt1;
                std::memcpy(&vt0, chunk.data() + o, 8);
                if (vt0 != vtable)
                    continue;
                std::memcpy(&vt1, chunk.data() + o + 0x10, 8);
                if (vt1 != vtable)
                    continue;
                s32 v0, v1;
                std::memcpy(&v0, chunk.data() + o + 8, 4);
                std::memcpy(&v1, chunk.data() + o + 0x18, 4);
                if (v0 != v1)
                    continue; // the pair (current, previous) agrees while the item is held
                for (std::size_t k = 0; k < 3; ++k)
                    if (!held[k].id.empty() && held[k].entries.size() < 4 && v0 == life[k]) {
                        held[k].entries.push_back(at + o);
                        held[k].entries.push_back(at + o + 0x10);
                    }
            }
            chunk.resize(1u << 20);
        }
        scan_cursor = stop;
        if (stop >= end) {
            // A full pass is done: whatever was not found is not on the heap right now.
            for (auto& g : held) {
                if (!g.id.empty() && !g.searched)
                    TotkNetLog::Log("durability: %s -> %zu entries", g.id.c_str(), g.entries.size());
                g.searched = !g.id.empty();
                g.searched_at = std::chrono::steady_clock::now();
            }
            scan_cursor = begin;
        }
    }

    // The Spawn page: a catalog per tab, and Add puts the chosen item in the game's pouch the
    // way a pickup does: more of a stack already held, else the name and amount in a free slot.
    template <class G>
    void PublishSpawn(const EdenDsmodHostApi& h, G& gmd) {
        if (names_ready.load(std::memory_order_acquire) && spawn_catalog[0].empty()) {
            for (const auto& [id, text] : names) {
                if (text.name.empty() || text.caption.empty())
                    continue;
                if (std::any_of(std::begin(SpawnSkip), std::end(SpawnSkip),
                                [&](std::string_view p) { return id.starts_with(p); }))
                    continue;
                for (std::size_t t = 0; t < std::size(SpawnTabs); ++t)
                    if (std::any_of(SpawnTabs[t].prefixes.begin(), SpawnTabs[t].prefixes.end(),
                                    [&](std::string_view p) { return !p.empty() && id.starts_with(p); }))
                        spawn_catalog[t].push_back(id);
            }
            for (auto& list : spawn_catalog) {
                std::sort(list.begin(), list.end());
                if (list.size() > MaxSpawnItems)
                    list.resize(MaxSpawnItems);
            }
            TotkNetLog::Log("spawn catalog: %zu weapons, %zu bows, %zu shields, %zu materials",
                            spawn_catalog[0].size(), spawn_catalog[1].size(), spawn_catalog[2].size(),
                            spawn_catalog[3].size());
        }
        int tab, selected;
        s32 amount;
        std::optional<std::pair<int, int>> request;
        {
            std::scoped_lock lock{mutex};
            tab = spawn_tab;
            selected = spawn_selected;
            amount = spawn_amount;
            request = std::exchange(spawn_request, std::nullopt);
        }
        const auto& list = spawn_catalog[static_cast<std::size_t>(tab)];
        if (selected >= static_cast<int>(list.size()))
            selected = list.empty() ? 0 : static_cast<int>(list.size()) - 1;
        if (request && request->first == tab && request->second == selected && !list.empty()) {
            const std::string result = Spawn(SpawnTabs[tab], list[static_cast<std::size_t>(selected)], amount);
            std::scoped_lock lock{mutex};
            spawn_result = result;
        }
        for (std::size_t t = 0; t < std::size(SpawnTabs); ++t)
            h.publish_i64(h.userdata, ("totk.spawn.tab." + std::to_string(t)).c_str(),
                          static_cast<int>(t) == tab ? 1 : 0);
        h.publish_i64(h.userdata, "totk.spawn.tab", tab);
        h.publish_text(h.userdata, "totk.spawn.title", SpawnTabs[tab].title);
        h.publish_i64(h.userdata, "totk.spawn.count", static_cast<s64>(list.size()));
        h.publish_text(h.userdata, "totk.spawn.empty_text",
                       list.empty() ? (names_ready.load() ? "Nothing found" : "Reading the game's item list...") : "");
        for (std::size_t r = 0; r < list.size(); ++r) {
            const std::string key = "totk.spawn.item." + std::to_string(r);
            h.publish_text(h.userdata, (key + ".icon").c_str(), ("module:totk:icon/" + list[r]).c_str());
            h.publish_i64(h.userdata, (key + ".selected").c_str(), static_cast<int>(r) == selected ? 1 : 0);
        }
        const bool valid = !list.empty();
        h.publish_i64(h.userdata, "totk.spawn.sel.valid", valid ? 1 : 0);
        h.publish_i64(h.userdata, "totk.spawn.sel.stack", valid && !SpawnTabs[tab].gear ? 1 : 0);
        h.publish_i64(h.userdata, "totk.spawn.can_add", valid && helper_alive && !helper_busy ? 1 : 0);
        if (!valid)
            return;
        const auto& id = list[static_cast<std::size_t>(selected)];
        h.publish_text(h.userdata, "totk.spawn.sel.icon", ("module:totk:icon/" + id).c_str());
        h.publish_text(h.userdata, "totk.spawn.sel.name", ItemText(id).c_str());
        h.publish_text(h.userdata, "totk.spawn.sel.desc", ItemCaption(id).c_str());
        std::string result;
        {
            std::scoped_lock lock{mutex};
            result = spawn_result;
        }
        h.publish_text(h.userdata, "totk.spawn.result", result.c_str());
    }

    // Hand the item to the game's own add-to-pouch (the helper): it finds the stack or a free
    // slot itself, with the item's default stats. The result arrives in PollHelper.
    std::string Spawn(const SpawnTab& tab, const std::string& id, s32 amount) {
        if (id.size() >= totk::mailbox::NameSize)
            return "Can't add that";
        if (!helper_alive)
            return "Adding items needs the game running";
        const s32 count = tab.gear ? 1 : std::clamp(amount, 1, MaxStack);
        if (!SendRequest(totk::mailbox::OpAdd, tab.category, static_cast<u32>(count), id))
            return "Busy, try again";
        pending_label = (count > 1 ? std::to_string(count) + " " : std::string{}) + ItemText(id);
        return "Adding...";
    }

    // The Cheats page: publish each toggle, then hold what is on. Writes go to the GameData
    // records the game itself keeps the values in.
    template <class G>
    void ApplyCheats(const EdenDsmodHostApi& h, G& gmd) {
        std::array<bool, NumCheats> on;
        {
            std::scoped_lock lock{mutex};
            on = cheats;
        }
        for (std::size_t i = 0; i < NumCheats; ++i)
            h.publish_text(h.userdata, ("totk.cheat." + std::to_string(i) + ".text").c_str(),
                           !CheatReady[i] ? "Coming soon" : on[i] ? "On" : "Off");
        h.publish_text(h.userdata, "totk.cheat.note",
                       "Cheats stay on until you turn them off or close the game. Durability and stamina can take a few seconds to take hold.");
        if (!h.write_memory)
            return;
        const auto hold_int = [&](u32 hash, s32 want) {
            if (const auto v = gmd.GetInt(hash); v && *v < want)
                if (const auto at = gmd.ScalarAddress(totk::gmd::Type::Int, hash))
                    h.write_memory(h.userdata, *at, &want, sizeof want);
        };
        if (on[CheatRupees])
            hold_int(Murmur3("PlayerStatus.CurrentRupee"), MaxRupees);
        if (on[CheatBattery]) {
            const u32 energy = Murmur3("PlayerStatus.Energy");
            const auto cur = gmd.GetFloat(energy);
            const auto max = gmd.GetFloat(Murmur3("PlayerStatus.MaxEnergy"));
            if (cur && max && std::isfinite(*max) && *max > 0 && *cur < *max)
                if (const auto at = gmd.ScalarAddress(totk::gmd::Type::Float, energy)) {
                    const float full = *max;
                    h.write_memory(h.userdata, *at, &full, sizeof full);
                }
        }
        if (on[CheatDurability])
            HoldDurability(h, gmd);
        else
            for (auto& g : held)
                g = {};
        if (on[CheatStamina])
            HoldStamina(h, gmd);
        if (on[CheatArrows]) {
            const auto names = gmd.GetString64Array(Murmur3("Pouch.Arrow.Content.Name"));
            const auto stock = gmd.GetIntArray(Murmur3("Pouch.Arrow.Content.StockNum"));
            for (std::size_t i = 0; names && stock && i < names->size() && i < stock->size(); ++i) {
                if ((*names)[i].empty() || (*stock)[i] >= MaxArrows)
                    continue;
                if (const auto at = gmd.IntArrayElement(Murmur3("Pouch.Arrow.Content.StockNum"), i)) {
                    const s32 full = MaxArrows;
                    h.write_memory(h.userdata, *at, &full, sizeof full);
                }
            }
        }
    }

    // Pouch.<Cat>.EquipIndex[0] for weapon, bow and shield, the part array for armor.
    static std::optional<u32> EquipKey(const Pouch& pouch, const std::string& id) {
        if (pouch.equip == Equip::Single)
            return Murmur3(std::string{"Pouch."} + pouch.name + ".EquipIndex");
        if (pouch.equip == Equip::Armor)
            for (const auto& part : ArmorParts)
                if (id.ends_with(part.suffix))
                    return part.hash;
        return std::nullopt;
    }

    template <class G>
    void PublishInventory(const EdenDsmodHostApi& h, G& gmd) {
        int category, selected;
        std::optional<std::pair<int, int>> use;
        {
            std::scoped_lock lock{mutex};
            category = inv_category;
            selected = inv_selected;
            use = std::exchange(inv_use, std::nullopt);
        }
        const auto& cat = Categories[category];
        for (std::size_t t = 0; t < std::size(Categories); ++t)
            h.publish_i64(h.userdata, ("totk.inv.tab." + std::to_string(t)).c_str(),
                          static_cast<int>(t) == category ? 1 : 0);
        h.publish_i64(h.userdata, "totk.inv.category", category);
        h.publish_text(h.userdata, "totk.inv.title", cat.title);

        // Pouch slots in order, skipping empty ones; `slot` keeps the game's index.
        struct Row {
            const Pouch* pouch;
            std::size_t slot;
            std::string id;
            std::optional<s32> amount;
            std::optional<u32> equip_key;
            bool equipped;
        };
        std::vector<Row> rows;
        std::unordered_map<u32, int> worn; // equip key -> equipped slot
        for (const auto& pouch : cat.pouches) {
            if (!pouch.name)
                continue;
            const std::string base = std::string{"Pouch."} + pouch.name;
            const auto names = gmd.GetString64Array(Murmur3(base + ".Content.Name"));
            if (!names)
                continue;
            const auto amounts =
                gmd.GetIntArray(Murmur3(base + (pouch.gear ? ".Content.Life" : ".Content.StockNum")));
            for (std::size_t i = 0; i < names->size() && rows.size() < MaxItems; ++i) {
                const std::string& id = (*names)[i];
                if (id.empty())
                    continue;
                Row row{&pouch, i, id, std::nullopt, EquipKey(pouch, id), false};
                if (amounts && i < amounts->size())
                    row.amount = (*amounts)[i];
                if (row.equip_key) {
                    auto it = worn.find(*row.equip_key);
                    if (it == worn.end()) {
                        const auto e = gmd.GetIntArray(*row.equip_key);
                        it = worn.emplace(*row.equip_key, e && !e->empty() ? (*e)[0] : -1).first;
                    }
                    row.equipped = it->second == static_cast<int>(i);
                }
                rows.push_back(std::move(row));
            }
        }
        h.publish_i64(h.userdata, "totk.inv.count", static_cast<s64>(rows.size()));
        h.publish_text(h.userdata, "totk.inv.empty_text", rows.empty() ? "Nothing here yet" : "");
        if (selected >= static_cast<int>(rows.size()))
            selected = rows.empty() ? 0 : static_cast<int>(rows.size()) - 1;

        // Equip / take off through the pause menu's own calls (the helper), so the held model
        // follows. Only for the row the button was pressed on, still in place; the equipped mark
        // updates from the save data once the game has done it.
        if (use && use->first == category && use->second == selected && !rows.empty()) {
            const auto& row = rows[static_cast<std::size_t>(selected)];
            if (IsFood(*row.pouch)) {
                if (SendRequest(totk::mailbox::OpEat, 6, static_cast<u32>(row.slot), {}))
                    equip_message.clear();
            } else if (const auto cat = PouchCategory(*row.pouch); cat && row.equip_key) {
                const u32 op = row.equipped ? totk::mailbox::OpUnequip : totk::mailbox::OpEquip;
                if (SendRequest(op, *cat, static_cast<u32>(row.slot), {}))
                    equip_message.clear();
            }
        }

        for (std::size_t r = 0; r < rows.size(); ++r) {
            const auto& row = rows[r];
            const std::string key = "totk.inv.item." + std::to_string(r);
            h.publish_text(h.userdata, (key + ".icon").c_str(), ("module:totk:icon/" + row.id).c_str());
            std::string qty;
            if (row.amount)
                qty = row.pouch->gear ? DurabilityText(*row.amount)
                                      : (*row.amount > 1 ? "x" + std::to_string(*row.amount) : "");
            h.publish_text(h.userdata, (key + ".qty").c_str(), qty.c_str());
            h.publish_i64(h.userdata, (key + ".equipped").c_str(), row.equipped ? 1 : 0);
            h.publish_i64(h.userdata, (key + ".selected").c_str(),
                          static_cast<int>(r) == selected ? 1 : 0);
        }
        const bool sel_valid = !rows.empty();
        h.publish_i64(h.userdata, "totk.inv.sel.valid", sel_valid ? 1 : 0);
        if (!sel_valid) {
            h.publish_i64(h.userdata, "totk.inv.sel.can_use", 0);
            return;
        }
        const auto& row = rows[static_cast<std::size_t>(selected)];
        const bool ready = helper_alive && !helper_busy;
        const bool can_eat = ready && IsFood(*row.pouch) && row.amount && *row.amount > 0;
        const bool can_equip = ready && row.equip_key && PouchCategory(*row.pouch);
        h.publish_i64(h.userdata, "totk.inv.sel.can_use", can_equip || can_eat ? 1 : 0);
        h.publish_text(h.userdata, "totk.inv.sel.use_text",
                       can_eat ? "Eat" : can_equip ? (row.equipped ? "Unequip" : "Equip") : "");
        h.publish_text(h.userdata, "totk.inv.sel.icon", ("module:totk:icon/" + row.id).c_str());
        h.publish_text(h.userdata, "totk.inv.sel.name", ItemText(row.id).c_str());
        h.publish_text(h.userdata, "totk.inv.sel.desc", ItemCaption(row.id).c_str());
        std::string stat;
        if (row.amount)
            stat = row.pouch->gear ? "Durability " + DurabilityText(*row.amount)
                                   : (*row.amount > 0 ? "Owned " + std::to_string(*row.amount) : "");
        if (!equip_message.empty())
            stat = equip_message;
        h.publish_text(h.userdata, "totk.inv.sel.stat", stat.c_str());
    }

    template <class G>
    void PublishOptions(const EdenDsmodHostApi& h, G& gmd) {
        for (std::size_t i = 0; i < std::size(Options); ++i) {
            const auto& row = Options[i];
            std::string_view text = "-";
            if (row.hash) {
                if (row.values.empty()) {
                    if (const auto v = gmd.GetBool(row.hash))
                        text = *v ? "On" : "Off";
                } else if (const auto v = gmd.GetEnum(row.hash)) {
                    for (const auto& value : row.values)
                        if (Murmur3(value.id) == *v)
                            text = value.text;
                }
            }
            const std::string key = "totk.option." + std::to_string(i) + ".text";
            h.publish_text(h.userdata, key.c_str(), std::string{text}.c_str());
        }
    }

    template <class G>
    void PublishVitals(const EdenDsmodHostApi& h, G& gmd) {
        const auto life = gmd.GetInt(Murmur3("PlayerStatus.Life"));
        const auto max_life = gmd.GetInt(Murmur3("PlayerStatus.MaxLife"));
        if (life && max_life && *max_life > 0) {
            // Quarter hearts, as the game counts them: "29¾", and the maximum underneath.
            constexpr const char* Quarters[]{"", "\u00BC", "\u00BD", "\u00BE"};
            const std::string hearts = std::to_string(*life / 4) + Quarters[*life % 4];
            const std::string of = "of " + std::to_string(*max_life / 4);
            h.publish_text(h.userdata, "totk.ready.hearts", hearts.c_str());
            h.publish_text(h.userdata, "totk.ready.hearts_of", of.c_str());
            h.publish_f64(h.userdata, "totk.ready.life_pct",
                          std::clamp(*life * 100.0 / *max_life, 0.0, 100.0));
        }
        // Stamina and battery: 1000 per wheel / per battery cell.
        const auto amount = [&](const char* key, const char* cur) {
            if (const auto c = gmd.GetFloat(Murmur3(cur)); c && std::isfinite(*c)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.1f", std::max(0.0f, *c) / 1000.0f);
                h.publish_text(h.userdata, key, buf);
            }
        };
        if (const auto rupees = gmd.GetInt(Murmur3("PlayerStatus.CurrentRupee")); rupees && *rupees >= 0) {
            // "347,549", grouped like the game's own counter.
            std::string digits = std::to_string(*rupees);
            for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
                digits.insert(static_cast<std::size_t>(at), ",");
            h.publish_text(h.userdata, "totk.ready.rupees_text", digits.c_str());
            h.publish_i64(h.userdata, "totk.ready.rupees.valid", 1);
        }
        amount("totk.ready.stamina_text", "PlayerStatus.Stamina");
        amount("totk.ready.battery_text", "PlayerStatus.Energy");
        if (const auto c = gmd.GetFloat(Murmur3("PlayerStatus.Stamina")),
            m = gmd.GetFloat(Murmur3("PlayerStatus.MaxStamina"));
            c && m && *m > 0 && std::isfinite(*c))
            h.publish_i64(h.userdata, "totk.ready.stamina_frame",
                          std::lround(std::clamp(*c / *m, 0.0f, 1.0f) * 50.0f));
        const auto pct = [&](const char* key, const char* cur, const char* max) {
            const auto c = gmd.GetFloat(Murmur3(cur));
            const auto m = gmd.GetFloat(Murmur3(max));
            if (c && m && *m > 0 && std::isfinite(*c))
                h.publish_f64(h.userdata, key, std::clamp(*c / *m * 100.0f, 0.0f, 100.0f));
        };
        pct("totk.ready.battery_pct", "PlayerStatus.Energy", "PlayerStatus.MaxEnergy");
    }

    template <class G>
    void PublishGear(const EdenDsmodHostApi& h, G& gmd) {
        for (const auto [slot, pouch] : {std::pair{"weapon", "Weapon"}, std::pair{"bow", "Bow"},
                                         std::pair{"shield", "Shield"}}) {
            const std::string base = std::string{"Pouch."} + pouch;
            const std::string key = std::string{"totk.ready."} + slot;
            const auto names = gmd.GetString64Array(Murmur3(base + ".Content.Name"));
            const auto lives = gmd.GetIntArray(Murmur3(base + ".Content.Life"));
            const auto equip = gmd.GetIntArray(Murmur3(base + ".EquipIndex"));
            int index = -1;
            if (equip && !equip->empty())
                index = (*equip)[0];
            else if (const auto e = gmd.GetInt(Murmur3(base + ".EquipIndex")))
                index = *e;
            const bool valid = names && index >= 0 && static_cast<std::size_t>(index) < names->size() &&
                               !(*names)[static_cast<std::size_t>(index)].empty();
            h.publish_i64(h.userdata, (key + ".valid").c_str(), valid ? 1 : 0);
            if (!valid) {
                h.publish_text(h.userdata, (key + ".name").c_str(), "None");
                h.publish_text(h.userdata, (key + ".durability_text").c_str(), "");
                continue;
            }
            const auto i = static_cast<std::size_t>(index);
            h.publish_text(h.userdata, (key + ".name").c_str(), ItemText((*names)[i]).c_str());
            h.publish_text(h.userdata, (key + ".icon").c_str(),
                           ("module:totk:icon/" + (*names)[i]).c_str());
            std::string durability;
            if (lives && i < lives->size()) {
                // Save editors mark gear unbreakable with a huge Life (2,100,000,000 + n).
                const s32 v = (*lives)[i];
                if (v >= 2'000'000'000)
                    durability = "MAX";
                else if (v >= 0)
                    durability = std::to_string(v);
            }
            h.publish_text(h.userdata, (key + ".durability_text").c_str(), durability.c_str());
        }
        // Arrows: TOTK has one arrow type (elements come from fusing), so the pouch's stock.
        const auto arrow_names = gmd.GetString64Array(Murmur3("Pouch.Arrow.Content.Name"));
        if (const auto stock = gmd.GetIntArray(Murmur3("Pouch.Arrow.Content.StockNum"));
            stock && arrow_names) {
            s64 arrows = 0;
            for (std::size_t i = 0; i < stock->size() && i < arrow_names->size(); ++i)
                if (!(*arrow_names)[i].empty())
                    arrows += std::max<s32>((*stock)[i], 0);
            h.publish_i64(h.userdata, "totk.ready.arrows", arrows);
            h.publish_i64(h.userdata, "totk.ready.arrows.valid", 1);
        }
        if (const auto powers = gmd.GetString64Array(Murmur3("Pouch.SpecialPower.Content.Name"))) {
            h.publish_text(h.userdata, "totk.ready.abilities_lines", AbilityText(*powers).c_str());
            std::size_t n = 0;
            for (const auto& id : *powers) {
                if (id.empty() || n == MaxAbilities)
                    continue;
                const auto* a = FindAbility(id);
                const std::string key = "totk.ready.ability." + std::to_string(n++);
                h.publish_text(h.userdata, (key + ".name").c_str(),
                               a ? std::string{a->name}.c_str() : id.c_str());
                h.publish_text(h.userdata, (key + ".icon").c_str(),
                               ("module:totk:icon/" + std::string{a ? a->icon : id}).c_str());
            }
            h.publish_i64(h.userdata, "totk.ready.ability.count", static_cast<s64>(n));
        }
    }

    template <class G>
    void PublishMap(const EdenDsmodHostApi& h, G& gmd) {
        const auto pos = gmd.GetVec3(Murmur3("PlayerStatus.CurrentPos"));
        if (!pos || !std::isfinite(pos->x) || !std::isfinite(pos->z)) {
            h.publish_i64(h.userdata, "totk.map.valid", 0);
            return;
        }
        int layer;
        {
            std::scoped_lock lock{mutex};
            static const std::vector<u8> no_sky;
            const auto& sky = world_ready.load(std::memory_order_acquire) ? world.sky : no_sky;
            const int now = LayerAt(pos->x, pos->y, pos->z, sky);
            if (now != link_layer)
                view_layer = -1; // Link changed layer: follow him again
            link_layer = now;
            layer = view_layer < 0 ? link_layer : view_layer;
        }
        h.publish_i64(h.userdata, "totk.map.valid", 1);
        h.publish_f64(h.userdata, "totk.map.x", pos->x);
        h.publish_f64(h.userdata, "totk.map.y", -pos->z);
        h.publish_i64(h.userdata, "totk.map.layer", layer);
        h.publish_text(h.userdata, "totk.map.layer_name", LayerNames[layer]);
        char coords[64];
        std::snprintf(coords, sizeof coords, "%.0f, %.0f, %.0f", pos->x, pos->y, pos->z);
        h.publish_text(h.userdata, "totk.map.coords_text", coords);
        // Every sample starts a fresh snapshot, so all 240 tile keys go out each time; the
        // strings are rebuilt only when the layer on show changes.
        if (layer != tiles_layer) {
            for (int i = 0; i < Cells; ++i) {
                near_tiles[i] = TileKey(layer, i, NearZoom, lit_signature[i]);
                far_tiles[i] = TileKey(layer, i, FarZoom, lit_signature[i]);
            }
            tiles_layer = layer;
        }
        for (int i = 0; i < Cells; ++i) {
            h.publish_text(h.userdata, TileBind("near", i).c_str(), near_tiles[i].c_str());
            h.publish_text(h.userdata, TileBind("far", i).c_str(), far_tiles[i].c_str());
        }
    }

    // The map's markers on the layer on show. First the travel points: a shrine shows once found
    // (DungeonState past Hidden), a lightroot once lit and a tower once activated; with their
    // category on in the Markers panel the others show too, dimmed. Then, on the Map page, the
    // all-markers layer's categories that are on, kept to the part of the map in view (and the
    // dense ones to closer zooms) so a sample never publishes more than the slots.
    template <class G>
    void PublishTravel(const EdenDsmodHostApi& h, G& gmd) {
        namespace mk = totk::markers;
        static const u32 Hidden = Murmur3("Hidden"), Open = Murmur3("Open"), Enter = Murmur3("Enter"),
                         Clear = Murmur3("Clear");
        int layer, selected, request, marker_sel;
        std::array<bool, mk::NumCategories> on;
        bool found_too, panel;
        {
            std::scoped_lock lock{mutex};
            layer = view_layer < 0 ? link_layer : view_layer;
            selected = travel_selected;
            marker_sel = marker_selected;
            request = std::exchange(travel_request, -1);
            on = marker_on;
            found_too = show_found;
            panel = markers_panel;
        }
        PublishMarkersPanel(h, on, found_too, panel);
        std::vector<int> slots;
        std::string sel_name, sel_status;
        bool sel_shown = false, sel_travel = false;
        const auto publish_slot = [&](int kind, float x, float z, const char* icon, s64 tint, bool dim) {
            const std::string key = "totk.travel." + std::to_string(slots.size() - 1);
            h.publish_i64(h.userdata, (key + ".kind").c_str(), kind);
            h.publish_f64(h.userdata, (key + ".x").c_str(), x);
            h.publish_f64(h.userdata, (key + ".y").c_str(), -z);
            h.publish_text(h.userdata, (key + ".icon").c_str(), icon);
            h.publish_i64(h.userdata, (key + ".tint").c_str(), tint);
            h.publish_i64(h.userdata, (key + ".dim").c_str(), dim ? 1 : 0);
        };
        if (world_ready.load(std::memory_order_acquire)) {
            const auto towers = gmd.GetBoolArray(0x8C826FCFu);
            for (std::size_t i = 0; i < world.points.size() && slots.size() < MaxTravel; ++i) {
                const auto& p = world.points[i];
                if (p.layer != layer)
                    continue;
                using totk::travel::Kind;
                const auto state = p.state ? gmd.GetEnum(p.state) : std::nullopt;
                bool shown = true, travel = false;
                std::string status;
                if (p.kind == Kind::Shrine) {
                    shown = state && *state != Hidden;
                    travel = state && (*state == Open || *state == Enter || *state == Clear);
                    status = !state ? "" : *state == Clear ? "Completed" : travel ? "Visited"
                                         : shown ? "Not yet entered" : "Not found yet";
                    if (const auto sub = ItemCaption(p.label); !sub.empty())
                        status = sub + "  -  " + status;
                } else if (p.kind == Kind::Tower) {
                    // BoolArray 8C826FCF: bit n-1 = TowerNN activated (all 15 set on a save
                    // with every tower up; the order is the label order).
                    const int n = p.label.size() == 7 ? std::atoi(p.label.c_str() + 5) : 0;
                    shown = n >= 1 && towers && static_cast<std::size_t>(n) <= towers->size() &&
                            (*towers)[static_cast<std::size_t>(n - 1)];
                    travel = shown;
                    status = shown ? "Activated" : "Not activated yet";
                } else if (p.kind == Kind::Lightroot) {
                    shown = state && *state == Open;
                    travel = shown;
                    status = shown ? "Lit" : "Not lit yet";
                }
                if (!shown && !on[static_cast<std::size_t>(p.kind)])
                    continue;
                slots.push_back(static_cast<int>(i));
                publish_slot(1 + static_cast<int>(p.kind), p.x, p.z, MarkerIcons[static_cast<int>(p.kind)],
                             MarkerTints[static_cast<int>(p.kind)], !travel);
                if (static_cast<int>(i) == selected) {
                    sel_shown = true;
                    sel_travel = travel;
                    sel_status = status;
                    sel_name = TravelName(p);
                }
            }
            RefreshMarkerDone(gmd);
            // The Map page's view as last drawn: centre, canvas px per world unit and size.
            const char* page = h.get_text ? h.get_text(h.userdata, "__page") : nullptr;
            const auto view = [&](const char* field) {
                return h.get_f64 ? h.get_f64(h.userdata, (std::string{"@map_view_"} + field + ":Hyrule").c_str(),
                                             std::nan(""))
                                 : std::nan("");
            };
            const double cx = view("cx"), cy = view("cy"), ppw = view("ppw"), vw = view("w"), vh = view("h");
            const bool any = std::any_of(on.begin() + 3, on.end(), [](bool b) { return b; });
            if (any && page && std::string_view{page} == "map" && std::isfinite(cx) && std::isfinite(cy) &&
                std::isfinite(ppw) && ppw > 0 && std::isfinite(vw) && std::isfinite(vh)) {
                // A little past the edges, so markers are there before a pan reaches them.
                const double hw = vw * 0.6 / ppw, hh = vh * 0.6 / ppw;
                for (std::size_t i = 0; i < world.markers.size() && slots.size() < MaxTravel; ++i) {
                    const auto& m = world.markers[i];
                    const auto c = static_cast<std::size_t>(m.cat);
                    if (!on[c] || m.layer != layer || ppw < MarkerMinPpw[c])
                        continue;
                    if (std::abs(m.x - cx) > hw || std::abs(-m.z - cy) > hh)
                        continue;
                    const bool done = i < marker_done.size() && marker_done[i];
                    if (done && !found_too && static_cast<int>(i) != marker_sel)
                        continue;
                    slots.push_back(-1 - static_cast<int>(i));
                    publish_slot(1 + static_cast<int>(c), m.x, m.z, MarkerIcons[c], MarkerTints[c], done);
                    if (static_cast<int>(i) == marker_sel) {
                        sel_shown = true;
                        sel_name = MarkerName(m);
                        sel_status = MarkerStatus(m, done);
                    }
                }
            }
        }
        {
            std::scoped_lock lock{mutex};
            travel_slots = std::move(slots);
            // A selection whose marker is gone (layer, panel or view changed) is dropped.
            if (!sel_shown && travel_selected == selected)
                travel_selected = -1;
            if (!sel_shown && marker_selected == marker_sel)
                marker_selected = -1;
        }
        if (marker_sel >= 0)
            selected = -1; // a layer marker is no travel destination
        h.publish_i64(h.userdata, "totk.travel.sel.valid", sel_shown ? 1 : 0);
        h.publish_text(h.userdata, "totk.travel.sel.name", sel_name.c_str());
        h.publish_text(h.userdata, "totk.travel.sel.status", sel_status.c_str());
        // A loading screen follows an accepted travel; the game ignores input until it's over.
        const bool loading = travel_landed.time_since_epoch().count() != 0 &&
                             std::chrono::steady_clock::now() - travel_landed < std::chrono::seconds(30);
        if (request >= 0 && request == selected && sel_travel && !loading)
            SendTravel(world.points[static_cast<std::size_t>(request)]);
        const bool ready = helper_alive && !helper_busy && !loading;
        h.publish_i64(h.userdata, "totk.travel.sel.can_travel", sel_travel && ready ? 1 : 0);
        std::string hint = marker_sel >= 0 ? ""
                           : !sel_travel ? "You can't travel here yet"
                           : (helper_busy && pending_op == totk::mailbox::OpTravel) || loading ? (travel_message.empty() ? "Traveling..." : travel_message)
                           : !helper_alive ? "Travel needs the game running with this mod loaded"
                           : travel_message;
        h.publish_text(h.userdata, "totk.travel.sel.hint", hint.c_str());
    }

    // The Markers panel: each row's icon, colour and state.
    void PublishMarkersPanel(const EdenDsmodHostApi& h, const std::array<bool, totk::markers::NumCategories>& on,
                             bool found_too, bool panel) {
        h.publish_i64(h.userdata, "totk.markers.panel", panel ? 1 : 0);
        if (!panel)
            return;
        const auto n = std::count(on.begin(), on.end(), true);
        h.publish_text(h.userdata, "totk.markers.0.text",
                       n == 0 ? "Off" : n == static_cast<long>(on.size()) ? "On" : "Some");
        for (std::size_t c = 0; c < on.size(); ++c) {
            const std::string key = "totk.markers." + std::to_string(c + 1);
            // Shrines, lightroots and towers you have found always show; the row adds the rest.
            const char* text = c < 3 ? (on[c] ? "All" : "Found") : on[c] ? "On" : "Off";
            h.publish_text(h.userdata, (key + ".text").c_str(), text);
            h.publish_text(h.userdata, (key + ".icon").c_str(), MarkerIcons[c]);
            h.publish_i64(h.userdata, (key + ".tint").c_str(), on[c] || c < 3 ? MarkerTints[c] : 0x80FFFCC6);
        }
        h.publish_text(h.userdata, ("totk.markers." + std::to_string(on.size() + 1) + ".text").c_str(),
                       found_too ? "On" : "Off");
    }

    // Found koroks and defeated bosses, a slice of the flagged markers per sample.
    template <class G>
    void RefreshMarkerDone(G& gmd) {
        namespace mk = totk::markers;
        constexpr std::size_t Slice = 256;
        const std::size_t n = world.markers.size();
        if (n == 0 || marker_done.size() != n)
            return;
        static const u32 Clear = Murmur3("Clear");
        for (std::size_t k = 0, checked = 0; k < n && checked < Slice; ++k) {
            const std::size_t i = done_cursor;
            done_cursor = (done_cursor + 1) % n;
            const auto& m = world.markers[i];
            if (m.done == mk::Done::None)
                continue;
            ++checked;
            if (m.done == mk::Done::Bool)
                marker_done[i] = gmd.GetBool(m.flag).value_or(false) ? 1 : 0;
            else if (const auto v = gmd.GetEnum(m.flag))
                marker_done[i] = *v == Clear ? 1 : 0;
        }
    }

    // A layer marker's name in the popup: the game's own where it has one.
    std::string MarkerName(const totk::markers::Marker& m) const {
        using totk::markers::Category;
        switch (m.cat) {
        case Category::Korok:
            return m.label == "KorokCarry" ? "Korok friend" : "Korok seed";
        case Category::Boss:
            if (const auto name = ItemText(m.label); name != m.label)
                return name;
            return std::string{totk::markers::BossKind(m.label)};
        case Category::Bubbul:
            if (const auto name = ItemText(m.label); name != m.label)
                return name;
            return "Bubbulfrog";
        case Category::Poe:
            return m.label == "MinusRupee_02" ? "Grand Poe" : m.label == "MinusRupee_01" ? "Large Poe" : "Poe";
        default:
            break;
        }
        if (const auto name = ItemText(m.label); name != m.label)
            return name;
        return m.label;
    }

    std::string MarkerStatus(const totk::markers::Marker& m, bool done) const {
        using totk::markers::Category;
        const bool flagged = m.done != totk::markers::Done::None;
        switch (m.cat) {
        case Category::Korok:
            return !flagged ? "Korok" : done ? "Found" : m.label == "KorokCarry" ? "Not reunited yet" : "Not found yet";
        case Category::Boss: {
            const std::string kind{totk::markers::BossKind(m.label)};
            return !flagged ? kind : kind + (done ? "  -  Defeated" : "  -  Not defeated yet");
        }
        case Category::Stable:
            return "Stable";
        case Category::Town:
            return "Town";
        case Category::Cave:
            return m.label.starts_with("Well_") ? "Well" : m.label.starts_with("DeepHole_") ? "Chasm" : "Cave";
        case Category::Bubbul:
            return "Bubbul gem";
        case Category::Poe:
            return "Poe";
        case Category::Weapon:
            return "Weapon";
        default:
            return "";
        }
    }

    // The Ready page's dials: shrines completed, lightroots lit and towers activated, each of
    // all there are.
    template <class G>
    void PublishProgress(const EdenDsmodHostApi& h, G& gmd) {
        if (!world_ready.load(std::memory_order_acquire))
            return;
        static const u32 Open = Murmur3("Open"), Clear = Murmur3("Clear");
        using totk::travel::Kind;
        std::array<int, 3> done{}, total{};
        const auto towers = gmd.GetBoolArray(0x8C826FCFu);
        for (const auto& p : world.points) {
            const int k = static_cast<int>(p.kind);
            ++total[k];
            if (p.kind == Kind::Tower) {
                const int n = p.label.size() == 7 ? std::atoi(p.label.c_str() + 5) : 0;
                done[k] += n >= 1 && towers && static_cast<std::size_t>(n) <= towers->size() &&
                           (*towers)[static_cast<std::size_t>(n - 1)];
            } else if (p.state) {
                const auto st = gmd.GetEnum(p.state);
                done[k] += st && *st == (p.kind == Kind::Shrine ? Clear : Open);
            }
        }
        constexpr const char* Keys[]{"totk.ready.shrines", "totk.ready.lightroots", "totk.ready.towers"};
        for (int k = 0; k < 3; ++k) {
            h.publish_i64(h.userdata, Keys[k], done[k]);
            h.publish_text(h.userdata, (std::string{Keys[k]} + "_text").c_str(),
                           (std::to_string(done[k]) + "/" + std::to_string(total[k])).c_str());
        }
    }

    // The game's place name ("Tauyosipun Shrine", "Stakijat Lightroot"), until the message
    // archive is in a plain one.
    std::string TravelName(const TravelPoint& p) const {
        using totk::travel::Kind;
        if (const auto name = ItemText(p.label); name != p.label)
            return name;
        if (p.kind == Kind::Tower)
            return "Skyview Tower";
        const std::string_view digits = std::string_view{p.label}.substr(
            std::min<std::size_t>(p.label.size(), p.kind == Kind::Shrine ? 7 : 10));
        return (p.kind == Kind::Shrine ? "Shrine " : "Lightroot ") + std::string{digits};
    }

    // Which lightroots are lit, and for each Depths cell a signature of the lit roots near it.
    template <class G>
    void UpdateLitRoots(G& gmd) {
        if (!world_ready.load(std::memory_order_acquire))
            return;
        static const u32 Open = Murmur3("Open");
        std::vector<std::pair<float, float>> lit;
        std::array<u32, Cells> sig{};
        constexpr float Reach = LitRadius + LitFeather;
        for (std::size_t i = 0; i < world.points.size(); ++i) {
            const auto& p = world.points[i];
            if (p.kind != totk::travel::Kind::Lightroot || !p.state)
                continue;
            if (const auto st = gmd.GetEnum(p.state); !st || *st != Open)
                continue;
            lit.emplace_back(p.x, p.z);
            const int c0 = static_cast<int>(std::floor((p.x - Reach + 6000.0f) / 1000.0f));
            const int c1 = static_cast<int>(std::floor((p.x + Reach + 6000.0f) / 1000.0f));
            const int r0 = static_cast<int>(std::floor((p.z - Reach + 5000.0f) / 1000.0f));
            const int r1 = static_cast<int>(std::floor((p.z + Reach + 5000.0f) / 1000.0f));
            for (int r = std::max(r0, 0); r <= std::min(r1, Rows - 1); ++r)
                for (int c = std::max(c0, 0); c <= std::min(c1, Cols - 1); ++c) {
                    u32& h = sig[static_cast<std::size_t>(r * Cols + c)];
                    h = (h ^ static_cast<u32>(i + 1)) * 0x01000193u;
                }
        }
        if (sig == lit_signature && !lit_signature_unset)
            return;
        {
            std::scoped_lock lock{lit_mutex};
            lit_roots = std::move(lit);
        }
        lit_signature = sig;
        lit_signature_unset = false;
        tiles_layer = -1; // rebuild the tile keys
        TotkNetLog::Log("depths: %zu lit lightroots", lit_roots.size());
    }

    // A Depths tile, dark except within LitRadius of a lit root (soft edge of LitFeather).
    std::shared_ptr<const TotkAssets::Image> LitDepthsTile(const TotkAssets::RomfsReader& romfs,
                                                           std::string_view logical) {
        const auto tilde = logical.find('~');
        const auto base = assets.Load(romfs, logical.substr(0, tilde));
        if (!base || base->width == 0 || base->height == 0)
            return base;
        // "map/U_CC-RR_ZZ": the cell's world box.
        const auto name = logical.substr(4);
        const int col = std::atoi(std::string{name.substr(2, 2)}.c_str());
        const int row = std::atoi(std::string{name.substr(5, 2)}.c_str());
        const float x0 = -6000.0f + 1000.0f * col, z0 = -5000.0f + 1000.0f * row;
        std::vector<std::pair<float, float>> roots;
        {
            std::scoped_lock lock{lit_mutex};
            for (const auto& [x, z] : lit_roots)
                if (x > x0 - LitRadius - LitFeather && x < x0 + 1000.0f + LitRadius + LitFeather &&
                    z > z0 - LitRadius - LitFeather && z < z0 + 1000.0f + LitRadius + LitFeather)
                    roots.emplace_back(x, z);
        }
        auto out = std::make_shared<TotkAssets::Image>(*base);
        const float sx = 1000.0f / static_cast<float>(base->width);
        const float sz = 1000.0f / static_cast<float>(base->height);
        for (u32 v = 0; v < base->height; ++v) {
            const float z = z0 + (static_cast<float>(v) + 0.5f) * sz;
            for (u32 u = 0; u < base->width; ++u) {
                const float x = x0 + (static_cast<float>(u) + 0.5f) * sx;
                float light = 0.0f;
                for (const auto& [rx, rz] : roots) {
                    const float d = std::sqrt((x - rx) * (x - rx) + (z - rz) * (z - rz));
                    light = std::max(light, std::clamp((LitRadius + LitFeather - d) / LitFeather, 0.0f, 1.0f));
                    if (light >= 1.0f)
                        break;
                }
                const float k = DarkLevel + (1.0f - DarkLevel) * light;
                u8* px = out->rgba.data() + (static_cast<std::size_t>(v) * base->width + u) * 4;
                for (int c = 0; c < 3; ++c)
                    px[c] = static_cast<u8>(static_cast<float>(px[c]) * k);
            }
        }
        return out;
    }

    static const std::string& TileBind(std::string_view zoom, int i) {
        static const auto make = [](std::string_view z) {
            std::array<std::string, Cells> out;
            for (int c = 0; c < Cells; ++c)
                out[c] = "totk.map.tile." + std::string{z} + "." + std::to_string(c);
            return out;
        };
        static const auto near = make("near"), far = make("far");
        return (zoom == "near" ? near : far)[static_cast<std::size_t>(i)];
    }

    int tiles_layer{-1};
    // Lit lightroots (x, z): written by the sample thread, read by the asset worker.
    std::mutex lit_mutex;
    std::vector<std::pair<float, float>> lit_roots;
    std::array<u32, Cells> lit_signature{};
    bool lit_signature_unset{true};
    std::array<std::string, Cells> near_tiles, far_tiles;
};

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        TotkNetLog::Start();
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId) {
            TotkNetLog::Log("create REFUSED: host abi %u size %u hash 0x%llx (module abi %u hash 0x%llx), "
                            "title %016llx",
                            host ? host->abi_version : 0u, host ? host->struct_size : 0u,
                            static_cast<unsigned long long>(host ? host->abi_hash : 0),
                            EDEN_DSMOD_MODULE_ABI_VERSION,
                            static_cast<unsigned long long>(EDEN_DSMOD_MODULE_ABI_HASH),
                            static_cast<unsigned long long>(host ? host->title_id : 0));
            TotkNetLog::Stop();
            return nullptr;
        }
        auto* m = new Module();
        m->supported = std::memcmp(host->build_id, BuildId, sizeof BuildId) == 0;
        m->main_base = host->main_base;
        char build[41]{};
        for (std::size_t i = 0; i < 20; ++i)
            std::snprintf(build + i * 2, 3, "%02X", host->build_id[i]);
        TotkNetLog::Log("create: build %s (%s), main 0x%llx size 0x%llx, caps 0x%llx, write %d, romfs %d",
                        build, m->supported ? "supported" : "NOT supported",
                        static_cast<unsigned long long>(host->main_base),
                        static_cast<unsigned long long>(host->main_size),
                        static_cast<unsigned long long>(host->capabilities), host->write_memory ? 1 : 0,
                        host->read_romfs ? 1 : 0);
        return m;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    TotkNetLog::Log("destroy");
    delete static_cast<Module*>(p);
    TotkNetLog::Stop();
}

void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (!p || !host)
            return;
        auto* m = static_cast<Module*>(p);
        host->begin_output(host->userdata);
        host->publish_i64(host->userdata, "totk.module", 1);
        host->publish_i64(host->userdata, "totk.supported", m->supported ? 1 : 0);
        host->publish_i64(host->userdata, "totk.unsupported", m->supported ? 0 : 1);
        if (m->supported) {
            m->Sample(*host);
        } else {
            host->publish_i64(host->userdata, "totk.ready", 0);
            // Which build is running, so a mismatch report says what the game actually is.
            char text[64] = "Running build ";
            for (std::size_t i = 0; i < 8; ++i)
                std::snprintf(text + 14 + i * 2, 3, "%02X", host->build_id[i]);
            host->publish_text(host->userdata, "totk.build_text", text);
        }
        host->end_output(host->userdata);
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "TOTK DSMod sample callback failed");
        TotkNetLog::Log("sample callback threw");
    }
}

void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p)
            static_cast<Module*>(p)->Configure(ext);
    } catch (...) {
    }
}

EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t arg) {
    try {
        if (p && action)
            return static_cast<Module*>(p)->OnAction(action, arg);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

// Map tiles and icons are decoded from the player's own romfs; nothing from the game is shipped.
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->LoadImage(host, key, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

// "module:totk:font": a marker the host hands back to decode_font once the atlas is built.
EdenDsmodBool LoadDataCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                               void* receiver, EdenDsmodDataSink sink) {
    try {
        if (!p || !key || !sink || std::string_view{key} != "module:totk:font")
            return EDEN_DSMOD_FALSE;
        if (!static_cast<Module*>(p)->Font(host))
            return EDEN_DSMOD_FALSE;
        sink(receiver, FontMarker, sizeof FontMarker);
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, std::size_t size,
                                 void* receiver, EdenDsmodFontSink sink) {
    try {
        if (!p || !bytes || !sink || size != sizeof FontMarker ||
            std::memcmp(bytes, FontMarker, size) != 0)
            return EDEN_DSMOD_FALSE;
        const auto* atlas = static_cast<Module*>(p)->Font(nullptr);
        if (!atlas)
            return EDEN_DSMOD_FALSE;
        sink(receiver, atlas->line_height, atlas->first_codepoint, atlas->glyphs.data(),
             static_cast<std::uint32_t>(atlas->glyphs.size()));
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Tears of the Kingdom DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
const EdenDsmodModuleDataExtensions DataExtensions{EDEN_DSMOD_DATA_EXT_VERSION,
                                                   sizeof(EdenDsmodModuleDataExtensions),
                                                   EDEN_DSMOD_DATA_EXT_HASH, &LoadDataCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
DSMOD_SDK_EXPORT_DATA_EXTENSIONS(DataExtensions)
