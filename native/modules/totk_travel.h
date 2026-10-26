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

inline std::vector<Point> Scan(std::span<const std::uint8_t> scene) {
    std::vector<Point> out;
    const byml::Document doc{scene};
    const auto actors = doc.Root()["Actors"];
    for (std::uint32_t i = 0, n = actors.Size(); i < n; ++i) {
        const auto actor = actors[i];
        const auto gyaml = actor["Gyaml"].String();
        if (!gyaml)
            continue;
        Point p;
        if (*gyaml == "DgnObj_ZonauShrineEntrance_A_01") {
            p.kind = Kind::Shrine;
            if (const auto s = actor["Dynamic"]["DungeonIndexStr"].String())
                p.state_key = "DungeonState." + std::string{*s};
        } else if (*gyaml == "FldObj_MinusFieldCheckPoint_A_01") {
            p.kind = Kind::Lightroot;
            if (const auto s = actor["Dynamic"]["CheckPointNo"].String())
                p.state_key = "ArrivalPointState." + std::string{*s};
        } else if (*gyaml == "DgnObj_WarpPoint_A_01") {
            p.kind = Kind::Tower;
        } else {
            continue;
        }
        const auto t = actor["Translate"];
        const auto x = t[0].Number(), y = t[1].Number(), z = t[2].Number();
        if (!x || !y || !z)
            continue;
        p.x = static_cast<float>(*x);
        p.y = static_cast<float>(*y);
        p.z = static_cast<float>(*z);
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace totk::travel
