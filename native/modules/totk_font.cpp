// SPDX-License-Identifier: GPL-3.0-or-later
// Atlas packing follows igawa6/eden-duo-companions native/modules/echoes_fonts.cpp (GPL-3.0).

#include "totk_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

namespace TotkFont {
namespace {
constexpr std::uint32_t Key = 0x028501A6u;
constexpr std::size_t MaxFontBytes = 16 * 1024 * 1024;
constexpr std::uint32_t AtlasWidth = 1024, MaxAtlasHeight = 4096;

std::uint32_t Le32(const std::uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
} // namespace

std::optional<std::vector<std::uint8_t>> Deobfuscate(std::span<const std::uint8_t> in) {
    if (in.size() < 12)
        return std::nullopt;
    // Word 0 is the BFOTF magic 0x18029A7F; word 1, the payload size, is big-endian once unmasked.
    if ((Le32(in.data()) ^ Key) != 0x18029A7Fu)
        return std::nullopt;
    const std::uint32_t size = __builtin_bswap32(Le32(in.data() + 4) ^ Key);
    if (size < 4 || size > MaxFontBytes || size > in.size() - 8)
        return std::nullopt;
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; i += 4) {
        std::uint8_t word[4]{};
        const std::size_t n = std::min<std::size_t>(4, in.size() - 8 - i);
        std::memcpy(word, in.data() + 8 + i, n);
        const std::uint32_t v = Le32(word) ^ Key;
        for (std::size_t b = 0; b < 4 && i + b < size; ++b)
            out[i + b] = static_cast<std::uint8_t>(v >> (8 * b));
    }
    // OpenType: CFF ("OTTO") or TrueType outlines.
    if (std::memcmp(out.data(), "OTTO", 4) != 0 && Le32(out.data()) != 0x00000100u)
        return std::nullopt;
    return out;
}

std::optional<Atlas> Build(std::span<const std::uint8_t> otf, std::uint32_t first,
                           std::uint32_t last, std::uint32_t cap_height) {
    if (otf.empty() || last < first || last - first > 0x1000 || cap_height < 8 ||
        cap_height > 128)
        return std::nullopt;
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, otf.data(), stbtt_GetFontOffsetForIndex(otf.data(), 0)))
        return std::nullopt;
    int hx0{}, hy0{}, hx1{}, hy1{};
    if (!stbtt_GetCodepointBox(&info, 'H', &hx0, &hy0, &hx1, &hy1) || hy1 <= hy0)
        return std::nullopt;
    const float scale = static_cast<float>(cap_height) / static_cast<float>(hy1 - hy0);
    struct Raster {
        std::uint32_t cp;
        int x0, y0, w, h, advance;
        std::uint32_t x, y;
    };
    std::vector<Raster> glyphs;
    std::uint32_t x = 1, y = 1, shelf = 0;
    for (std::uint32_t cp = first; cp <= last; ++cp) {
        if (!stbtt_FindGlyphIndex(&info, static_cast<int>(cp)))
            continue;
        int x0{}, y0{}, x1{}, y1{}, advance{}, bearing{};
        stbtt_GetCodepointBitmapBox(&info, static_cast<int>(cp), scale, scale, &x0, &y0, &x1, &y1);
        stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &advance, &bearing);
        if (x1 < x0 || y1 < y0 || x1 - x0 > 512 || y1 - y0 > 512)
            continue;
        const auto w = static_cast<std::uint32_t>(x1 - x0), h = static_cast<std::uint32_t>(y1 - y0);
        if (x + w + 1 > AtlasWidth) {
            x = 1;
            y += shelf;
            shelf = 0;
        }
        if (y + h + 1 > MaxAtlasHeight)
            return std::nullopt;
        glyphs.push_back({cp, x0, y0, static_cast<int>(w), static_cast<int>(h),
                          static_cast<int>(std::lround(advance * scale)), x, y});
        x += w + 2;
        shelf = std::max(shelf, h + 2);
    }
    if (glyphs.empty())
        return std::nullopt;
    Atlas out;
    const std::uint32_t height = (y + shelf + 3) / 4 * 4;
    out.image.width = AtlasWidth;
    out.image.height = height;
    out.image.rgba.assign(static_cast<std::size_t>(AtlasWidth) * height * 4, 0);
    for (std::size_t i = 0; i < out.image.rgba.size(); i += 4)
        out.image.rgba[i] = out.image.rgba[i + 1] = out.image.rgba[i + 2] = 255;
    out.first_codepoint = first;
    out.line_height = cap_height;
    out.glyphs.assign(last - first + 1, EdenDsmodFontGlyph{});
    std::vector<std::uint8_t> mask;
    for (const auto& g : glyphs) {
        mask.assign(static_cast<std::size_t>(g.w) * g.h, 0);
        if (g.w && g.h)
            stbtt_MakeCodepointBitmap(&info, mask.data(), g.w, g.h, g.w, scale, scale,
                                      static_cast<int>(g.cp));
        for (int iy = 0; iy < g.h; ++iy)
            for (int ix = 0; ix < g.w; ++ix)
                out.image.rgba[((static_cast<std::size_t>(g.y) + iy) * AtlasWidth + g.x + ix) * 4 +
                               3] = mask[static_cast<std::size_t>(iy) * g.w + ix];
        out.glyphs[g.cp - first] = {static_cast<std::uint16_t>(g.x),
                                    static_cast<std::uint16_t>(g.y),
                                    static_cast<std::uint16_t>(g.w),
                                    static_cast<std::uint16_t>(g.h),
                                    static_cast<std::int16_t>(g.x0),
                                    static_cast<std::int16_t>(-g.y0),
                                    static_cast<std::uint16_t>(std::max(0, g.advance))};
    }
    return out;
}

std::optional<Atlas> LoadGameFont(const TotkAssets::RomfsReader& read, std::string_view member,
                                  std::uint32_t cap_height) {
    TotkAssets::Dictionaries dicts;
    if (const auto pack = read("/Pack/ZsDic.pack.zs"))
        dicts.Load(*pack);
    const auto raw = read("/Font/Font.Nin_NX_NVN.bfarc.zs");
    if (!raw)
        return std::nullopt;
    const auto sarc = dicts.Decompress(*raw);
    if (!sarc)
        return std::nullopt;
    const auto bfotf = TotkAssets::SarcMember(*sarc, member);
    if (!bfotf)
        return std::nullopt;
    const auto otf = Deobfuscate(*bfotf);
    if (!otf)
        return std::nullopt;
    // Latin-1: everything English item names and the companion's own text use.
    return Build(*otf, 0x20, 0xFF, cap_height);
}

} // namespace TotkFont
