// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace TotkAssets {

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

struct AstcDecoder {
    void* userdata{};
    EdenDsmodAstcDecoder decode{};
};

/// Reads a romfs file (the module passes host->read_romfs; the test tool reads a dump).
using RomfsReader = std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)>;

/// The game's zstd dictionaries, from /Pack/ZsDic.pack.zs (a plain zstd SARC of *.zsdic).
/// Most TOTK romfs files are zstd frames compressed against one of them.
class Dictionaries {
public:
    Dictionaries();
    ~Dictionaries();
    Dictionaries(const Dictionaries&) = delete;
    Dictionaries& operator=(const Dictionaries&) = delete;
    /// Load every *.zsdic member of the ZsDic pack; false when the pack is missing or bad.
    bool Load(std::span<const std::uint8_t> zsdic_pack);
    bool Empty() const;
    /// zstd frame -> bytes using the dictionary the frame names (or none); input returned
    /// unchanged when it is not a zstd frame.
    std::optional<std::vector<std::uint8_t>> Decompress(std::span<const std::uint8_t> in) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// zstd frame without a dictionary -> bytes (input returned unchanged when it is not zstd).
std::optional<std::vector<std::uint8_t>> Zstd(std::span<const std::uint8_t> in);
/// SARC member by name.
std::optional<std::vector<std::uint8_t>> SarcMember(std::span<const std::uint8_t> sarc,
                                                    std::string_view name);
std::vector<std::string> SarcNames(std::span<const std::uint8_t> sarc);
/// MSBT label -> text (UTF-8, inline control tags dropped).
std::vector<std::pair<std::string, std::string>> MsbtEntries(std::span<const std::uint8_t> msbt);
/// Display text of actors (items, weapons, materials) from /Mals/<lang>.Product.*.sarc.zs:
/// every ActorMsg/*.msbt "<Actor>_Name" and "<Actor>_Caption", keyed by actor name; plus place
/// names from LocationMsg: "DungeonNNN" (shrine name, caption = its subtitle), "CheckPointNNN"
/// (lightroots), "TowerNN" (Skyview towers) and the other Location.msbt labels.
struct ActorText {
    std::string name, caption;
};
std::unordered_map<std::string, ActorText> LoadActorNames(const RomfsReader& read,
                                                          std::string_view lang);
/// Texture names in a BNTX.
std::vector<std::string> BntxNames(std::span<const std::uint8_t> bntx);
/// Decode one BNTX texture (by name; empty name = the first texture) to RGBA8.
std::optional<Image> DecodeBntx(std::span<const std::uint8_t> bntx, std::string_view texture,
                                AstcDecoder astc);

/// Logical image keys served to the manifest as module:totk:<key>:
///   map/<L>_<CC>-<RR>_<ZZ>[_<flags>]  a map tile, /UI/Map/MainField/<same>.bntx.zs; L is G
///                                     (Surface), S (Sky) or U (Depths), ZZ the zoom 00..04
///   icon/<Actor>                      an inventory icon, /UI/Tex/Icon/<Actor>.bntx.zs
///   lyt/<Archive>/<Tex>               a texture from /UI/LayoutArchive/<Archive>.blarc.zs
///                                     timg/__Combined.bntx (icons, frames)
/// Thread-safe, cached (LRU by bytes). Nothing decoded is persisted or shipped.
class Decoder {
public:
    explicit Decoder(std::size_t cache_budget = 48 * 1024 * 1024);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    std::shared_ptr<const Image> Load(const RomfsReader& read, std::string_view key);
    void SetAstcDecoder(AstcDecoder decoder);
    /// romfs path + texture name a key resolves to (empty when the key is invalid).
    static std::optional<std::pair<std::string, std::string>> Resolve(std::string_view key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace TotkAssets
