// SPDX-License-Identifier: GPL-3.0-or-later

// The all-markers map layer: koroks, bosses, stables, towns, caves, Bubbulfrogs, poes and
// field weapons, read once from the player's own romfs (nothing extracted ships with the
// package). Places come from Banc/<Field>/LocationArea, koroks from Banc/MainField/HiddenKorok,
// the rest from the scene files' actors. Koroks and bosses carry the GameData Bool (or Enum)
// that says they are done, resolved through GameDataList's Struct tables: the member key is
// murmur3 of the actor's instance hash written in decimal.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <tuple>
#include <vector>

#include "totk_byml.h"
#include "totk_gamedata.h"

namespace totk::markers {

/// In the Markers panel's order (tools/totk/gen_manifest.py MARKER_ROWS, after "All markers").
/// Shrines, lightroots and towers are the travel points; the rest are this layer's own.
enum class Category : std::uint8_t {
    Shrine,
    Lightroot,
    Tower,
    Korok,
    Boss,
    Stable,
    Town,
    Cave,
    Bubbul,
    Poe,
    Weapon,
    Count
};
constexpr std::size_t NumCategories = static_cast<std::size_t>(Category::Count);

/// How a marker knows it is done.
enum class Done : std::uint8_t { None, Bool, EnumClear };

struct Marker {
    Category cat{};
    std::uint8_t layer{}; // 0 Surface, 1 Sky, 2 Depths (the module's Layer)
    Done done{Done::None};
    float x{}, y{}, z{};
    std::uint32_t flag{};  // GameData Bool / Enum key hash when done != None
    std::string label;     // Location.msbt label (places) or actor name (Gyaml)
};

// Cave scenes (Banc/MainField/Cave/<name>_Static|_Dynamic), from the 1.4.2 romfs file list.
constexpr std::string_view CaveScenes[]{
    "Cave_Akkala_0000_GroupSet_000", "Cave_Akkala_0003_GroupSet_000", "Cave_Akkala_0005",
    "Cave_Akkala_0007_GroupSet_000", "Cave_Akkala_0010_GroupSet_000",
    "Cave_Akkala_0011_GroupSet_000", "Cave_Akkala_0014_GroupSet_000",
    "Cave_Akkala_0017_GroupSet_000", "Cave_CentralHyrule_0008_GroupSet_000",
    "Cave_CentralHyrule_0009_GroupSet_000", "Cave_CentralHyrule_0018_GroupSet_000",
    "Cave_CentralHyrule_0020_GroupSet_000", "Cave_CentralHyrule_0021_GroupSet_000",
    "Cave_CentralHyrule_0022_GroupSet_000", "Cave_CentralHyrule_0023_GroupSet_000",
    "Cave_CentralHyrule_0025_GroupSet_000", "Cave_CentralHyrule_0026_GroupSet_000",
    "Cave_Eldin_0020_GroupSet_000", "Cave_Eldin_0021_GroupSet_000", "Cave_Eldin_0022_GroupSet_000",
    "Cave_Eldin_0023_GroupSet_000", "Cave_Eldin_0024_GroupSet_000", "Cave_Eldin_0025_GroupSet_000",
    "Cave_Eldin_0026_GroupSet_000", "Cave_Eldin_0027_GroupSet_000", "Cave_Eldin_0028_GroupSet_000",
    "Cave_Eldin_0029_GroupSet_000", "Cave_Eldin_0030_GroupSet_000", "Cave_Eldin_0031_GroupSet_000",
    "Cave_Eldin_0033_GroupSet_000", "Cave_Eldin_0034_GroupSet_000", "Cave_Eldin_0035_GroupSet_000",
    "Cave_Eldin_0037_GroupSet_000", "Cave_Eldin_0039_GroupSet_000", "Cave_Eldin_GroupSet_000",
    "Cave_Firone_0002_GroupSet_000", "Cave_Firone_0004_GroupSet_000",
    "Cave_Firone_0005_GroupSet_000", "Cave_Firone_0008_GroupSet_000",
    "Cave_Firone_0009_GroupSet_000", "Cave_Firone_0016_GroupSet_000",
    "Cave_Firone_0019_GroupSet_000", "Cave_Firone_0020_GroupSet_000",
    "Cave_Firone_0021_GroupSet_000", "Cave_Firone_0022_GroupSet_000",
    "Cave_Firone_0023_GroupSet_000", "Cave_Firone_0024", "Cave_Firone_0029_GroupSet_000",
    "Cave_Firone_0032_GroupSet_000", "Cave_FirstPlateau_0001_GroupSet_000",
    "Cave_FirstPlateau_0007_GroupSet_000", "Cave_GerudoDesert_0007_GroupSet_000",
    "Cave_GerudoDesert_0008_GroupSet_000", "Cave_GerudoDesert_0015_GroupSet_000",
    "Cave_GerudoDesert_0022_GroupSet_000", "Cave_GerudoDesert_0030_GroupSet_000",
    "Cave_GerudoDesert_0031_GroupSet_000", "Cave_GerudoDesert_0032_GroupSet_000",
    "Cave_GerudoDesert_0033_GroupSet_000", "Cave_GerudoDesert_0034_GroupSet_000",
    "Cave_GerudoDesert_0035_GroupSet_000", "Cave_GerudoDesert_0036_GroupSet_000",
    "Cave_GerudoDesert_0037_GroupSet_000", "Cave_GerudoDesert_0039_GruopSet_000",
    "Cave_GerudoDesert_0040_GroupSet_000", "Cave_GerudoDesert_0041_GroupSet_000",
    "Cave_GerudoDesert_0044_GroupSet_000", "Cave_GerudoDesert_0045_GroupSet_000",
    "Cave_GerudoDesert_0046_GroupSet_000", "Cave_GerudoDesert_0049_GroupSet_001",
    "Cave_GerudoDesert_0050_GroupSet_000", "Cave_GerudoDesert_0051_GroupSet_000",
    "Cave_GerudoHighlands_0002_GroupSet_000", "Cave_GerudoHighlands_0008_GroupSet_000",
    "Cave_GerudoHighlands_0014", "Cave_GerudoHighlands_0017_GroupSet_000",
    "Cave_HateruEast_0000_GroupSet_000", "Cave_HateruEast_0002_GroupSet_000",
    "Cave_HateruEast_0006_GroupSet_000", "Cave_HateruEast_0007_GroupSet_000",
    "Cave_HateruEast_0008_GroupSet_000", "Cave_HateruEast_0009_GroupSet_000",
    "Cave_HateruEast_0013_GroupSet_000", "Cave_HateruEast_0014_GroupSet_000",
    "Cave_HateruEast_0016_GroupSet_000", "Cave_HateruWest_0002_GroupSet_000",
    "Cave_HateruWest_0005_GroupSet_000", "Cave_HateruWest_0006_GroupSet_000",
    "Cave_HateruWest_0008_GroupSet_000", "Cave_HateruWest_0011_GroupSet_000",
    "Cave_HateruWest_0012_GroupSet_000", "Cave_Hebra_0000_GroupSet_000", "Cave_Hebra_0013",
    "Cave_Hebra_0015_GroupSet_000", "Cave_Hebra_0016_GroupSet_000", "Cave_Hebra_0019_GroupSet_000",
    "Cave_Hebra_0021_GroupSet_000", "Cave_Hebra_0022_GroupSet_000", "Cave_Hebra_0023_GroupSet_000",
    "Cave_Hebra_0025_GroupSet_000", "Cave_Hebra_0026_GroupSet_000", "Cave_Hebra_0030_GroupSet_000",
    "Cave_Hebra_0035_GroupSet_000", "Cave_Hebra_0036_GroupSet_000", "Cave_Hebra_0037_GroupSet_000",
    "Cave_Hebra_0038_GroupSet_000", "Cave_Hebra_0039_GroupSet_000", "Cave_Hebra_0040",
    "Cave_Hebra_0042_GroupSet_000", "Cave_HyruleForest_0001_GroupSet_000",
    "Cave_HyruleForest_0006_GroupSet_000", "Cave_HyruleForest_0007_GroupSet_001",
    "Cave_HyruleForest_0008_GroupSet_000", "Cave_HyruleRidge_0000_GroupSet_000",
    "Cave_HyruleRidge_0002_GroupSet_000", "Cave_HyruleRidge_0003",
    "Cave_HyruleRidge_0005_GroupSet_000", "Cave_HyruleRidge_0006_GroupSet_000",
    "Cave_HyruleRidge_0007_GroupSet_000", "Cave_HyruleRidge_0008_GroupSet_000",
    "Cave_LanayruMountain_0002_GroupSet_000", "Cave_LanayruMountain_0004_GroupSet_000",
    "Cave_LanayruMountain_0006", "Cave_LanayruMountain_0008_GroupSet_000",
    "Cave_LanayruMountain_0010_GroupSet_000", "Cave_LanayruMountain_0014_GroupSet_000",
    "Cave_LanayruMountain_0016_GroupSet_000", "Cave_LanayruMountain_0022_GroupSet_000",
    "Cave_LanayruMountain_0024_GroupSet_000", "Cave_LanayruMountain_0026_GroupSet_000",
    "Cave_LanayruMountain_GroupSet_000", "Cave_Lanayru_0006_Group_Set_001",
    "Cave_Lanayru_0008_GroupSet_000", "Cave_Lanayru_0014_GroupSet_000",
    "Cave_Lanayru_0019_GroupSet_000", "Cave_Lanayru_0024_GroupSet_000",
    "Cave_Lanayru_0031_GroupSet_000", "Cave_Lanayru_0032_GroupSet_000",
    "Cave_Lanayru_0033_GroupSet_000", "Cave_Lanayru_0035_GroupSet_000",
    "Cave_Lanayru_0036_GroupSet_000", "Cave_Lanayru_0045_GroupSet_000",
    "Cave_Lanayru_0048_GroupSet_000", "Cave_Lanayru_0049_GroupSet_000",
    "Cave_Lanayru_0052_GroupSet_000", "Cave_Lanayru_0053_GroupSet_000",
    "Cave_Lanayru_0055_GroupSet_000", "Cave_Lanayru_0057_GroupSet_000",
    "Cave_Lanayru_0060_GroupSet_000", "Cave_Lanayru_0061", "Cave_Tabantha_0001_GroupSet_000",
    "Cave_Tabantha_0002_GroupSet_000", "Cave_Tabantha_0003_GroupSet_000",
    "Map_Fld_Zora_RetreatSet_B_01", "Pln_Fld_Zora_AncientFacility__01_01", "Well_0001_GroupSet_000",
    "Well_0002_GroupSet_000", "Well_0003_GroupSet_000", "Well_0005_GroupSet_000",
    "Well_0006_GroupSet_000", "Well_0007_GroupSet_000", "Well_0008_GroupSet_000",
    "Well_0010_GroupSet_000", "Well_0011_GroupSet_000", "Well_0012_GroupSet_000",
    "Well_0013_GroupSet_000", "Well_0014_GroupSet_000", "Well_0015_GroupSet_000",
    "Well_0016_GroupSet_000", "Well_0017_GroupSet_000", "Well_0018_GroupSet_000",
    "Well_0023_GroupSet_000", "Well_0024_GroupSet_000", "Well_0025_GroupSet_000", "Well_0026",
    "Well_0027_GroupSet_000", "Well_0028_GroupSet_000", "Well_0029_GroupSet_000",
    "Well_0030_GroupSet_000", "Well_0031_GroupSet_000", "Well_0032_GroupSet_000",
    "Well_0033_GroupSet_000", "Well_0034_GroupSet_000", "Well_0041_GroupSet_000",
    "Well_0042_GroupSet_000", "Well_0043_GroupSet_000", "Well_0044_GroupSet_000",
    "Well_0045_GroupSet_000", "Well_0046_GroupSet_000", "Well_0047_GroupSet_000",
    "Well_0048_GroupSet_000", "Well_0049_GroupSet_000", "Well_0050_GroupSet_000",
    "Well_0051_GroupSet_000", "Well_0052_GroupSet_000", "Well_0053_GroupSet_000",
    "Well_0054_GroupSet_000", "Well_0055_GroupSet_000", "Well_0056_GroupSet_000",
    "Well_0058_GroupSet_000", "Well_0059_GroupSet_000", "ZonauRuins_MiniCave_MineralSet_01",
};

/// GameData Struct tables that map instance hashes to completion flags.
constexpr std::uint32_t KorokStruct = 0xED6B585Du;      // hidden koroks -> Bool
constexpr std::uint32_t CarryKorokStruct = 0xD4B7BF27u; // carry koroks -> Enum NotClear/Clear
constexpr std::uint32_t BossStruct = 0xCD3EC27Eu;       // bosses defeated once -> Bool
constexpr std::uint32_t EnumClear = gmd::Murmur3("Clear");
static_assert(EnumClear == 1654019904u); // GameDataList: Values [NotClear, Clear]

using StructMembers = std::unordered_map<std::uint32_t, std::uint32_t>; // member key -> target

/// The members of the wanted Struct entries of GameDataList (big-endian BYML).
inline std::unordered_map<std::uint32_t, StructMembers> LoadStructs(std::span<const std::uint8_t> list,
                                                                     std::span<const std::uint32_t> wanted) {
    std::unordered_map<std::uint32_t, StructMembers> out;
    const byml::Document doc{list};
    const auto structs = doc.Root()["Data"]["Struct"];
    for (std::uint32_t i = 0, n = structs.Size(); i < n; ++i) {
        const auto entry = structs[i];
        const auto hash = entry["Hash"].Integer();
        if (!hash || std::find(wanted.begin(), wanted.end(), static_cast<std::uint32_t>(*hash)) == wanted.end())
            continue;
        auto& members = out[static_cast<std::uint32_t>(*hash)];
        const auto values = entry["DefaultValue"];
        for (std::uint32_t m = 0, mn = values.Size(); m < mn; ++m) {
            const auto key = values[m]["Hash"].Integer(), value = values[m]["Value"].Integer();
            if (key && value)
                members.emplace(static_cast<std::uint32_t>(*key), static_cast<std::uint32_t>(*value));
        }
    }
    return out;
}

/// The flag a Struct table gives an instance hash (0 = none).
inline std::uint32_t MemberFlag(const StructMembers& members, std::uint64_t instance) {
    const auto it = members.find(gmd::Murmur3(std::to_string(instance)));
    return it == members.end() ? 0 : it->second;
}

/// Category of a scene actor this layer shows (bosses, Bubbulfrogs, poes, field weapons).
inline std::optional<Category> ActorCategory(std::string_view gyaml) {
    if (gyaml.ends_with("_Far"))
        return std::nullopt;
    if (gyaml.starts_with("Weapon_"))
        return Category::Weapon;
    if (gyaml.starts_with("MinusRupee_0"))
        return Category::Poe;
    if (!gyaml.starts_with("Enemy_"))
        return std::nullopt;
    gyaml.remove_prefix(6);
    if (gyaml.starts_with("CaveMaster_"))
        return Category::Bubbul;
    for (const std::string_view boss : {"Giant_", "Golem_", "Sandworm", "Drake_", "Mogurudo_",
                                        "Zonau_BlockMaster_", "Lynel_"})
        if (gyaml.starts_with(boss)) {
            // Pebblits and baby Froxes are not bosses.
            if (gyaml.starts_with("Golem_Little") || gyaml.find("_Baby") != std::string_view::npos)
                return std::nullopt;
            return Category::Boss;
        }
    return std::nullopt;
}

/// A boss's kind when the message archive has no name for its actor.
inline std::string_view BossKind(std::string_view gyaml) {
    constexpr std::pair<std::string_view, std::string_view> kinds[]{
        {"Enemy_Giant_Bone", "Stalnox"},        {"Enemy_Giant_", "Hinox"},
        {"Enemy_Golem_Fire", "Igneo Talus"},    {"Enemy_Golem_Ice", "Frost Talus"},
        {"Enemy_Golem_", "Stone Talus"},        {"Enemy_Sandworm", "Molduga"},
        {"Enemy_Drake_Fire", "Flame Gleeok"},   {"Enemy_Drake_Ice", "Frost Gleeok"},
        {"Enemy_Drake_Electric", "Thunder Gleeok"}, {"Enemy_Drake_", "King Gleeok"},
        {"Enemy_Mogurudo_", "Frox"},            {"Enemy_Zonau_BlockMaster_", "Flux Construct"},
        {"Enemy_Lynel_", "Lynel"}};
    for (const auto& [prefix, name] : kinds)
        if (gyaml.starts_with(prefix))
            return name;
    return "Boss";
}

/// The place categories of a LocationArea file this layer shows.
inline std::optional<Category> PlaceCategory(std::string_view category, std::string_view label) {
    if (category == "Stable")
        return Category::Stable;
    if (category == "City")
        return Category::Town;
    if (category == "CaveEntranceNormal" || category == "CaveEntranceWell")
        return Category::Cave;
    if (category == "CaveEntranceSpecial" && label.starts_with("DeepHole_"))
        return Category::Cave; // chasms
    return std::nullopt;
}

/// "Work/Location/Cave_Akkala_0000.game__location__Location.gyml" -> "Cave_Akkala_0000".
inline std::string_view LocationLabel(std::string_view path) {
    if (const auto slash = path.rfind('/'); slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    if (const auto dot = path.find('.'); dot != std::string_view::npos)
        path = path.substr(0, dot);
    return path;
}

/// Stables, towns, caves, wells and chasms of a LocationArea file: {category: [{LocationName,
/// Trans: [[x, y, z]...], CaveMarkerInfo: [{Trans}]}]}. One marker per place (its marker point
/// for a cave), at the first of its instances. Walked generically: any dict holding a
/// LocationName is a place of the nearest enclosing dict key.
inline void ScanPlaces(std::span<const std::uint8_t> file, std::uint8_t layer, std::vector<Marker>& out) {
    const byml::Document doc{file};
    const auto visit = [&](const auto& self, const byml::Node& node, std::string_view category, int depth) -> void {
        if (depth > 6)
            return;
        if (node.IsArray()) {
            for (std::uint32_t i = 0, n = node.Size(); i < n; ++i)
                self(self, node[i], category, depth + 1);
            return;
        }
        if (!node.IsDict())
            return;
        const auto path = node["LocationName"].String();
        if (!path) {
            for (std::uint32_t i = 0, n = node.Size(); i < n; ++i) {
                const auto [key, value] = node.Entry(i);
                self(self, value, key, depth + 1);
            }
            return;
        }
        const auto label = LocationLabel(*path);
        const auto cat = PlaceCategory(category, label);
        if (!cat)
            return;
        auto at = node["CaveMarkerInfo"][0u]["Trans"];
        if (!at[0u].Number())
            at = node["Trans"][0u];
        const auto x = at[0u].Number(), y = at[1u].Number(), z = at[2u].Number();
        if (x && y && z)
            out.push_back({*cat, layer, Done::None, static_cast<float>(*x), static_cast<float>(*y),
                           static_cast<float>(*z), 0, std::string{label}});
    };
    visit(visit, doc.Root(), {}, 0);
}

/// Koroks of Banc/MainField/HiddenKorok: {HiddenKorok: {"<instance>": [x, y, z]}, CarryKorok: ...}.
inline void ScanKoroks(std::span<const std::uint8_t> file, const StructMembers* hidden,
                       const StructMembers* carry, std::vector<Marker>& out) {
    const byml::Document doc{file};
    const auto root = doc.Root();
    for (const auto& [group, members, done] :
         {std::tuple{"HiddenKorok", hidden, Done::Bool}, std::tuple{"CarryKorok", carry, Done::EnumClear}}) {
        const auto koroks = root[group];
        for (std::uint32_t i = 0, n = koroks.Size(); i < n; ++i) {
            const auto [id, at] = koroks.Entry(i);
            const auto x = at[0u].Number(), y = at[1u].Number(), z = at[2u].Number();
            if (!x || !y || !z)
                continue;
            std::uint64_t instance{};
            for (const char ch : id)
                instance = instance * 10 + static_cast<std::uint64_t>(ch - '0');
            const std::uint32_t flag = members ? MemberFlag(*members, instance) : 0;
            out.push_back({Category::Korok, 0, flag ? done : Done::None, static_cast<float>(*x),
                           static_cast<float>(*y), static_cast<float>(*z), flag,
                           group == std::string_view{"CarryKorok"} ? "KorokCarry" : "Korok"});
        }
    }
}

} // namespace totk::markers
