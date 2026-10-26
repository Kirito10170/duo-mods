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
#include "totk_netlog.h"
#include "totk_travel.h"

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
constexpr std::size_t MaxTravel = 400;
// Marker pictures from the Common layout archive, by travel::Kind.
constexpr const char* TravelIcons[]{"module:totk:common/MapIconDungeon_L_Icon^s",
                                    "module:totk:common/MapIconArrivalPoint_L_FrameIcon^s",
                                    "module:totk:common/MapIconTower_00^s"};

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
};

World LoadWorld(const TotkAssets::RomfsReader& read, const std::atomic<bool>& stop) {
    World world;
    world.sky.assign(static_cast<std::size_t>(SkyCols * SkyRows), 0);
    TotkAssets::Dictionaries dicts;
    if (const auto pack = read("/Pack/ZsDic.pack.zs"))
        dicts.Load(*pack);
    std::unordered_map<std::string, bool> seen;
    std::vector<totk::travel::TowerTag> tags;
    const auto scene = [&](const std::string& path, int layer) {
        if (stop.load(std::memory_order_relaxed))
            return;
        const auto raw = read(path);
        const auto bytes = raw ? dicts.Decompress(*raw) : std::nullopt;
        if (!bytes)
            return;
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
            for (int row = 1; row <= 8; ++row)
                scene(std::string{"/Banc/"} + field + "/" + col + "-" + std::to_string(row) +
                          "_Static.bcett.byml.zs",
                      layer);
    for (const auto name : SkyScenes)
        scene("/Banc/MainField/Sky/" + std::string{name} + "_Static.bcett.byml.zs", Sky);
    for (const auto name : ShrineScenes)
        scene("/Banc/MainField/" + std::string{name} + "_Static.bcett.byml.zs", Surface);
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
constexpr bool CheatReady[NumCheats]{false, true, true, true, false};
constexpr s32 MaxArrows = 999, MaxRupees = 999'999;

// ---- Item spawning ------------------------------------------------------------------------
// The Spawn page's tabs: which pouch an item goes into and which actor names belong there. The
// catalog is every actor with both a name and a description in the message archive (items
// have both; enemies and objects have no description), filtered by prefix.
struct SpawnTab {
    const char* title;
    const char* pouch;
    bool gear;
    std::array<std::string_view, 3> prefixes;
};
constexpr SpawnTab SpawnTabs[]{
    {"Weapons", "Weapon", true, {"Weapon_Sword_", "Weapon_Lsword_", "Weapon_Spear_"}},
    {"Bows", "Bow", true, {"Weapon_Bow_"}},
    {"Shields", "Shield", true, {"Weapon_Shield_"}},
    {"Materials", "Material", false, {"Item_", "Animal_Insect_"}},
};
// Cooked and roasted things go to the Food pouch with extra fields; they are not offered.
constexpr std::string_view SpawnSkip[]{"Item_Cook_", "Item_Roast_", "Item_Chilled_", "Item_Boiled_"};
constexpr s32 MaxStack = 999, SpawnLife = 40;
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
    int spawn_tab{0}, spawn_selected{0};
    std::optional<std::pair<int, int>> spawn_request; // {tab, row} of a pressed Add button
    s32 spawn_amount{1};
    std::string spawn_result;                   // what the last Add did, for the page
    std::array<std::vector<std::string>, std::size(SpawnTabs)> spawn_catalog; // after names
    std::mutex mutex;  // guards the view state below (actions arrive off the sample thread)
    std::chrono::steady_clock::time_point last_heartbeat{};
    int view_layer{-1}; // -1 = follow Link
    std::vector<int> travel_slots; // map marker slot -> world.points index
    int travel_selected{-1};       // world.points index
    int link_layer{Surface};

    void Configure(const EdenDsmodHostExtensions* ext) {
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext))
            assets.SetAstcDecoder({ext->userdata, ext->decode_astc});
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
            const int point = known ? travel_slots[static_cast<std::size_t>(arg)] : -1;
            travel_selected = travel_selected == point ? -1 : point;
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
            const std::string result = Spawn(h, gmd, SpawnTabs[tab], list[static_cast<std::size_t>(selected)], amount);
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
        h.publish_i64(h.userdata, "totk.spawn.can_add", valid && h.write_memory ? 1 : 0);
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

    template <class G>
    std::string Spawn(const EdenDsmodHostApi& h, G& gmd, const SpawnTab& tab, const std::string& id,
                      s32 amount) {
        if (!h.write_memory || id.size() >= 64)
            return "Can't add items here";
        const std::string base = std::string{"Pouch."} + tab.pouch;
        const u32 name_key = Murmur3(base + ".Content.Name");
        const u32 amount_key = Murmur3(base + (tab.gear ? ".Content.Life" : ".Content.StockNum"));
        const auto names_now = gmd.GetString64Array(name_key);
        const auto amounts = gmd.GetIntArray(amount_key);
        if (!names_now || !amounts)
            return "The pouch isn't ready";
        const auto write_s32 = [&](std::size_t slot, s32 v) {
            const auto at = gmd.IntArrayElement(amount_key, slot);
            return at && h.write_memory(h.userdata, *at, &v, sizeof v) == EDEN_DSMOD_TRUE;
        };
        // A stack already held grows (up to 999).
        if (!tab.gear)
            for (std::size_t i = 0; i < names_now->size() && i < amounts->size(); ++i)
                if ((*names_now)[i] == id) {
                    const s32 now = std::max<s32>((*amounts)[i], 0);
                    const s32 next = std::min(MaxStack, now + amount);
                    const bool ok = write_s32(i, next);
                    TotkNetLog::Log("spawn %s: stack slot %zu %d -> %d %s", id.c_str(), i, now, next,
                                    ok ? "ok" : "FAILED");
                    return ok ? "Added " + std::to_string(next - now) + " " + ItemText(id) : "Couldn't add it";
                }
        // Else the first free slot (no name), within the slots the player has unlocked.
        std::size_t limit = names_now->size();
        if (const auto valid = gmd.GetInt(Murmur3(base + ".ValidNum")); valid && *valid > 0)
            limit = std::min(limit, static_cast<std::size_t>(*valid));
        for (std::size_t i = 0; i < limit && i < amounts->size(); ++i) {
            if (!(*names_now)[i].empty())
                continue;
            const auto at = gmd.String64Element(name_key, i);
            if (!at)
                break;
            std::array<char, 64> text{};
            std::memcpy(text.data(), id.data(), id.size());
            // Amount first, so the game never sees the name with a free slot's -1.
            const s32 value = tab.gear ? SpawnLife : std::min(MaxStack, amount);
            const bool ok = write_s32(i, value) &&
                            h.write_memory(h.userdata, *at, text.data(), text.size()) == EDEN_DSMOD_TRUE;
            TotkNetLog::Log("spawn %s: new slot %zu (%d) %s", id.c_str(), i, value, ok ? "ok" : "FAILED");
            return ok ? "Added " + ItemText(id) : "Couldn't add it";
        }
        return "No room in your " + std::string{tab.title} + " pouch";
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
                       "Cheats marked Coming soon are still being mapped in the game's memory.");
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

        // Equip / take off: one aligned s32 store into the pouch's equip array, the same change
        // the pause menu makes. Only for the row the button was pressed on, still in place.
        if (use && use->first == category && use->second == selected && !rows.empty()) {
            auto& row = rows[static_cast<std::size_t>(selected)];
            if (row.equip_key && h.write_memory)
                if (const auto at = gmd.IntArrayElement(*row.equip_key, 0)) {
                    const s32 value = row.equipped ? -1 : static_cast<s32>(row.slot);
                    const bool ok = h.write_memory(h.userdata, *at, &value, sizeof value) == EDEN_DSMOD_TRUE;
                    TotkNetLog::Log("equip %s slot %zu -> %d: %s", row.id.c_str(), row.slot, value,
                                    ok ? "written" : "write FAILED");
                    if (ok)
                        for (auto& other : rows)
                            if (other.equip_key == row.equip_key)
                                other.equipped = value == static_cast<s32>(other.slot);
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
        const bool can_equip = row.equip_key && h.write_memory;
        h.publish_i64(h.userdata, "totk.inv.sel.can_use", can_equip ? 1 : 0);
        h.publish_text(h.userdata, "totk.inv.sel.use_text",
                       can_equip ? (row.equipped ? "Unequip" : "Equip") : "");
        h.publish_text(h.userdata, "totk.inv.sel.icon", ("module:totk:icon/" + row.id).c_str());
        h.publish_text(h.userdata, "totk.inv.sel.name", ItemText(row.id).c_str());
        h.publish_text(h.userdata, "totk.inv.sel.desc", ItemCaption(row.id).c_str());
        std::string stat;
        if (row.amount)
            stat = row.pouch->gear ? "Durability " + DurabilityText(*row.amount)
                                   : (*row.amount > 0 ? "Owned " + std::to_string(*row.amount) : "");
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

    // Shrine, lightroot and tower markers of the layer on show. A shrine shows once found
    // (DungeonState past Hidden) and a lightroot once lit; towers have no known state key yet.
    template <class G>
    void PublishTravel(const EdenDsmodHostApi& h, G& gmd) {
        static const u32 Hidden = Murmur3("Hidden"), Open = Murmur3("Open"), Enter = Murmur3("Enter"),
                         Clear = Murmur3("Clear");
        int layer, selected;
        {
            std::scoped_lock lock{mutex};
            layer = view_layer < 0 ? link_layer : view_layer;
            selected = travel_selected;
        }
        std::vector<int> slots;
        std::string sel_name, sel_status;
        bool sel_shown = false, sel_travel = false;
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
                    status = !state ? "" : *state == Clear ? "Completed" : travel ? "Visited" : "Not yet entered";
                    if (const auto sub = ItemCaption(p.label); !sub.empty())
                        status = sub + "  -  " + status;
                } else if (p.kind == Kind::Tower) {
                    // BoolArray 8C826FCF: bit n-1 = TowerNN activated (all 15 set on a save
                    // with every tower up; the order is the label order).
                    const int n = p.label.size() == 7 ? std::atoi(p.label.c_str() + 5) : 0;
                    shown = n >= 1 && towers && static_cast<std::size_t>(n) <= towers->size() &&
                            (*towers)[static_cast<std::size_t>(n - 1)];
                    travel = shown;
                    status = "Activated";
                } else if (p.kind == Kind::Lightroot) {
                    shown = state && *state == Open;
                    travel = shown;
                    status = "Lit";
                }
                if (!shown)
                    continue;
                const std::size_t slot = slots.size();
                slots.push_back(static_cast<int>(i));
                const std::string key = "totk.travel." + std::to_string(slot);
                h.publish_i64(h.userdata, (key + ".kind").c_str(), 1 + static_cast<int>(p.kind));
                h.publish_f64(h.userdata, (key + ".x").c_str(), p.x);
                h.publish_f64(h.userdata, (key + ".y").c_str(), -p.z);
                h.publish_text(h.userdata, (key + ".icon").c_str(), TravelIcons[static_cast<int>(p.kind)]);
                // The icons are white masks the game colours: shrine amber, lightroot green, tower blue.
                constexpr s64 Tints[]{0xFFF2A93B, 0xFF9BE36B, 0xFF5CC8F0};
                h.publish_i64(h.userdata, (key + ".tint").c_str(), Tints[static_cast<int>(p.kind)]);
                h.publish_i64(h.userdata, (key + ".dim").c_str(), travel ? 0 : 1);
                if (static_cast<int>(i) == selected) {
                    sel_shown = true;
                    sel_travel = travel;
                    sel_status = status;
                    sel_name = TravelName(p);
                }
            }
        }
        for (std::size_t slot = slots.size(); slot < MaxTravel; ++slot)
            h.publish_i64(h.userdata, ("totk.travel." + std::to_string(slot) + ".kind").c_str(), 0);
        {
            std::scoped_lock lock{mutex};
            travel_slots = std::move(slots);
            if (!sel_shown)
                travel_selected = -1;
        }
        h.publish_i64(h.userdata, "totk.travel.sel.valid", sel_shown ? 1 : 0);
        h.publish_text(h.userdata, "totk.travel.sel.name", sel_name.c_str());
        h.publish_text(h.userdata, "totk.travel.sel.status", sel_status.c_str());
        // The travel request itself needs the game's own warp call (a guest helper); until it
        // lands the panel says so instead of offering a button that does nothing.
        h.publish_i64(h.userdata, "totk.travel.sel.can_travel", 0);
        h.publish_text(h.userdata, "totk.travel.sel.hint",
                       sel_travel ? "Travel from here is coming in the next build" : "You can't travel here yet");
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
