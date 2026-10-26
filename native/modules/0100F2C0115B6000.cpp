// SPDX-License-Identifier: GPL-3.0-or-later

// The Legend of Zelda: Tears of the Kingdom module: ABI glue.
// Supported build: 1.4.2 (update v655360), main 5CB42B1CF25469FB0635FD046453D843C18BC8AB.
// Every other build loads too, but only publishes the "update required" state (no reads).
//
// Stage 1: the Ready page (vitals, equipped gear, abilities) and the world map, read from the
// game's GameDataMgr (totk_gamedata.h); map tiles and icons are decoded from the player's romfs
// (totk_assets.*).
//
// Android is the target: nothing here may rely on guest calls (NCE has none), so changes to
// game state go through write_memory or button presses only.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <span>
#include <string>
#include <string_view>

#include "totk_assets.h"
#include "totk_gamedata.h"

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
constexpr float SkyAbove = 1100.0f, DepthsBelow = -150.0f;

std::string TileKey(int layer, int cell, const char* zoom) {
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
    }
    return buf;
}

int LayerAt(float y) {
    return y > SkyAbove ? Sky : y < DepthsBelow ? Depths : Surface;
}

// ---- Ready page -----------------------------------------------------------------------------
struct AbilityName {
    std::string_view id, name;
};
constexpr AbilityName Abilities[]{{"UltraHand", "Ultrahand"}, {"Fuse", "Fuse"},
                                  {"Ascend", "Ascend"},       {"Recall", "Recall"},
                                  {"Autobuild", "Autobuild"}, {"Camera", "Camera"},
                                  {"Purah", "Purah Pad"}};

std::string AbilityText(const std::vector<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) {
        if (id.empty())
            continue;
        std::string_view name = id;
        for (const auto& a : Abilities)
            if (a.id == id)
                name = a.name;
        if (!out.empty())
            out += ", ";
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

struct Module {
    bool supported{};
    u64 main_base{};
    TotkAssets::Decoder assets;
    // Item display names, loaded once off the sample thread from the player's message archive.
    std::thread names_thread;
    std::atomic<bool> names_ready{false};
    std::unordered_map<std::string, std::string> names;

    ~Module() {
        if (names_thread.joinable())
            names_thread.join();
    }

    void StartNames(const EdenDsmodHostApi& h) {
        if (names_thread.joinable() || !h.read_romfs)
            return;
        names_thread = std::thread([this, romfs = MakeRomfsReader(h)] {
            try {
                names = TotkAssets::LoadActorNames(romfs, "USen");
            } catch (...) {
                names.clear();
            }
            names_ready.store(true, std::memory_order_release);
        });
    }

    // Internal ids (Weapon_Sword_052) until the names are in.
    std::string ItemText(const std::string& id) const {
        if (names_ready.load(std::memory_order_acquire))
            if (const auto it = names.find(id); it != names.end())
                return it->second;
        return id;
    }
    std::mutex mutex;  // guards the view state below (actions arrive off the sample thread)
    int view_layer{-1}; // -1 = follow Link
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
        const auto romfs = MakeRomfsReader(*host);
        std::shared_ptr<const TotkAssets::Image> image;
        if (logical == "player_marker") {
            // The update may renumber the layout archive; try the known spellings.
            for (const char* archive : {"Common.Product.142.Nin_NX_NVN", "Common.Product.140.Nin_NX_NVN",
                                        "Common.Product.120.Nin_NX_NVN", "Common.Product.110.Nin_NX_NVN",
                                        "Common.Product.100.Nin_NX_NVN"})
                if ((image = assets.Load(romfs, std::string{"lyt/"} + archive +
                                                    "/Nt_MapPlayerIconNew_00^s")))
                    break;
        } else {
            image = assets.Load(romfs, logical);
        }
        if (!image)
            return EDEN_DSMOD_FALSE;
        sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    EdenDsmodBool OnAction(std::string_view action) {
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
        totk::gmd::Reader gmd{read, h.main_base};
        const bool live = gmd.Begin();
        h.publish_i64(h.userdata, "totk.ready", live ? 1 : 0);
        if (!live) {
            h.publish_i64(h.userdata, "totk.map.valid", 0);
            return;
        }
        PublishVitals(h, gmd);
        PublishGear(h, gmd);
        PublishMap(h, gmd);
        PublishOptions(h, gmd);
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
            // Quarter hearts.
            char buf[48];
            const int whole = *life / 4, quarter = *life % 4;
            if (quarter)
                std::snprintf(buf, sizeof buf, "%d %d/4 of %d", whole, quarter, *max_life / 4);
            else
                std::snprintf(buf, sizeof buf, "%d of %d", whole, *max_life / 4);
            h.publish_text(h.userdata, "totk.ready.hearts_text", buf);
            h.publish_i64(h.userdata, "totk.ready.life", *life);
            h.publish_i64(h.userdata, "totk.ready.max_life", *max_life);
        }
        const auto pct = [&](const char* key, const char* cur, const char* max) {
            const auto c = gmd.GetFloat(Murmur3(cur));
            const auto m = gmd.GetFloat(Murmur3(max));
            if (c && m && *m > 0 && std::isfinite(*c))
                h.publish_f64(h.userdata, key, std::clamp(*c / *m * 100.0f, 0.0f, 100.0f));
        };
        pct("totk.ready.stamina_pct", "PlayerStatus.Stamina", "PlayerStatus.MaxStamina");
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
        if (const auto powers = gmd.GetString64Array(Murmur3("Pouch.SpecialPower.Content.Name")))
            h.publish_text(h.userdata, "totk.ready.abilities_text", AbilityText(*powers).c_str());
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
            const int now = LayerAt(pos->y);
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
                near_tiles[i] = TileKey(layer, i, NearZoom);
                far_tiles[i] = TileKey(layer, i, FarZoom);
            }
            tiles_layer = layer;
        }
        for (int i = 0; i < Cells; ++i) {
            h.publish_text(h.userdata, TileBind("near", i).c_str(), near_tiles[i].c_str());
            h.publish_text(h.userdata, TileBind("far", i).c_str(), far_tiles[i].c_str());
        }
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
    std::array<std::string, Cells> near_tiles, far_tiles;
};

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId)
            return nullptr;
        auto* m = new Module();
        m->supported = std::memcmp(host->build_id, BuildId, sizeof BuildId) == 0;
        m->main_base = host->main_base;
        return m;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    delete static_cast<Module*>(p);
}

void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (!p || !host)
            return;
        auto* m = static_cast<Module*>(p);
        host->begin_output(host->userdata);
        host->publish_i64(host->userdata, "totk.supported", m->supported ? 1 : 0);
        host->publish_i64(host->userdata, "totk.unsupported", m->supported ? 0 : 1);
        if (m->supported)
            m->Sample(*host);
        else
            host->publish_i64(host->userdata, "totk.ready", 0);
        host->end_output(host->userdata);
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "TOTK DSMod sample callback failed");
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

EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t) {
    try {
        if (p && action)
            return static_cast<Module*>(p)->OnAction(action);
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

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Tears of the Kingdom DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
