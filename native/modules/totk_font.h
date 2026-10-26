// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The game's own UI lettering for the companion: TOTK ships scalable OpenType (CFF) fonts in
// /Font/Font.Nin_NX_NVN.bfarc.zs (a zstd SARC), each XOR-obfuscated (.bfotf). This rasterises
// one into a glyph atlas the host draws labels from (font / font_atlas in the manifest).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "totk_assets.h"

namespace TotkFont {

struct Atlas {
    TotkAssets::Image image;          // white, coverage in alpha
    std::uint32_t first_codepoint{};  // glyphs[i] is first_codepoint + i
    std::uint32_t line_height{};      // the cap height the host scales to 5 px per text_scale
    std::vector<EdenDsmodFontGlyph> glyphs;
};

/// .bfotf -> OpenType bytes: u32 magic ^ key, u32 size ^ key, then the font XOR'd per LE u32.
std::optional<std::vector<std::uint8_t>> Deobfuscate(std::span<const std::uint8_t> bfotf);

/// Rasterise codepoints first..last of an OpenType font at the given cap height (in pixels).
std::optional<Atlas> Build(std::span<const std::uint8_t> otf, std::uint32_t first,
                           std::uint32_t last, std::uint32_t cap_height);

/// Load /Font/Font.Nin_NX_NVN.bfarc.zs member `member` and build its atlas.
std::optional<Atlas> LoadGameFont(const TotkAssets::RomfsReader& read, std::string_view member,
                                  std::uint32_t cap_height);

} // namespace TotkFont
