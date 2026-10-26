// SPDX-License-Identifier: GPL-3.0-or-later

// Travel points of the field, read from the player's own scene files (Banc/MainField/...
// .bcett.byml): shrines, lightroots and Skyview towers, with the GameData key that says whether
// each one is open. Nothing extracted ships with the package.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "totk_byml.h"

namespace totk::travel {

enum class Kind { Shrine, Lightroot, Tower };

struct Point {
    Kind kind{};
    float x{}, y{}, z{};
    std::string state_key; // "DungeonState.Dungeon021", "ArrivalPointState.CheckPoint086"; towers: ""
};

/// A Skyview tower's launch cannon: it names its tower ("Tower01") and stands next to the
/// tower's warp point, which itself carries no index.
struct TowerTag {
    std::string tower;
    float x{}, z{};
};

/// Every actor of a scene with a position: fn(gyaml, x, y, z, actor node).
template <class Fn>
void ForEachActor(std::span<const std::uint8_t> scene, Fn&& fn) {
    const byml::Document doc{scene};
    const auto actors = doc.Root()["Actors"];
    for (std::uint32_t i = 0, n = actors.Size(); i < n; ++i) {
        const auto actor = actors[i];
        const auto gyaml = actor["Gyaml"].String();
        const auto t = actor["Translate"];
        const auto x = t[0].Number(), y = t[1].Number(), z = t[2].Number();
        if (gyaml && x && y && z)
            fn(*gyaml, static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z), actor);
    }
}

inline std::vector<Point> Scan(std::span<const std::uint8_t> scene, std::vector<TowerTag>* tags = nullptr) {
    std::vector<Point> out;
    ForEachActor(scene, [&](std::string_view gyaml, float x, float y, float z, const byml::Node& actor) {
        if (tags && gyaml == "FldObj_Cannon_A_01")
            if (const auto t = actor["Dynamic"]["TowerNo"].String())
                tags->push_back({std::string{*t}, x, z});
        Point p{Kind::Shrine, x, y, z, {}};
        if (gyaml == "DgnObj_ZonauShrineEntrance_A_01") {
            if (const auto s = actor["Dynamic"]["DungeonIndexStr"].String())
                p.state_key = "DungeonState." + std::string{*s};
        } else if (gyaml == "FldObj_MinusFieldCheckPoint_A_01") {
            p.kind = Kind::Lightroot;
            if (const auto s = actor["Dynamic"]["CheckPointNo"].String())
                p.state_key = "ArrivalPointState." + std::string{*s};
        } else if (gyaml == "DgnObj_WarpPoint_A_01") {
            p.kind = Kind::Tower;
        } else {
            return;
        }
        out.push_back(std::move(p));
    });
    return out;
}

} // namespace totk::travel
