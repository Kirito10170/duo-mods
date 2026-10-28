// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side helper for the TOTK companion's asset decoder, run against a romfs dump:
//   totk-assets-tool <romfs> list <path>        SARC members, or BNTX texture names
//   totk-assets-tool <romfs> dump <key> <out>   decode a module:totk:<key> image to PAM (RGBA)
//   totk-assets-tool <romfs> msbt <path> [member] MSBT labels and text (member: inside a SARC)
//   totk-assets-tool <romfs> font <member> <out>  rasterise a Font.bfarc font to a PAM atlas
//   totk-assets-tool <romfs> travel <path>...     travel points in Banc scene files
// It exercises exactly the code the module ships. The emulator's ASTC decoder is not here, so
// an ASTC texture is written as <out>.astc (astcenc container) and the PAM comes out grey;
// `astcenc -dl <out>.astc <out>.png` decodes it for checking.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "totk_assets.h"
#include "totk_font.h"
#include "totk_travel.h"

using namespace TotkAssets;

static std::optional<std::vector<std::uint8_t>> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static std::string astc_out;

static EdenDsmodBool WriteAstc(void*, std::uint32_t w, std::uint32_t h, std::uint32_t bw,
                               std::uint32_t bh, const std::uint8_t* in, std::size_t in_size,
                               std::uint8_t* out, std::size_t out_size) {
    std::ofstream o(astc_out, std::ios::binary);
    const std::uint8_t header[16]{0x13, 0xAB, 0xA1, 0x5C, static_cast<std::uint8_t>(bw),
                                  static_cast<std::uint8_t>(bh), 1,
                                  static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8),
                                  static_cast<std::uint8_t>(w >> 16),
                                  static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(h >> 8),
                                  static_cast<std::uint8_t>(h >> 16), 1, 0, 0};
    o.write(reinterpret_cast<const char*>(header), sizeof header);
    o.write(reinterpret_cast<const char*>(in), static_cast<std::streamsize>(in_size));
    std::memset(out, 0x80, out_size);
    std::printf("astc %ux%u block %ux%u -> %s\n", w, h, bw, bh, astc_out.c_str());
    return o ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <romfs> list <path> | dump <key> <out.pam>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1], op = argv[2];
    const RomfsReader read = [&](const std::string& p) { return ReadFile(root + p); };
    if (op == "list") {
        Dictionaries dicts;
        if (const auto pack = read("/Pack/ZsDic.pack.zs"); !pack || !dicts.Load(*pack))
            std::fprintf(stderr, "warning: /Pack/ZsDic.pack.zs not loaded\n");
        const auto raw = read(argv[3]);
        if (!raw)
            return 1;
        const auto bytes = dicts.Decompress(*raw);
        if (!bytes) {
            std::fprintf(stderr, "decompress failed\n");
            return 1;
        }
        std::printf("size %zu\n", bytes->size());
        for (const auto& n : SarcNames(*bytes)) {
            std::printf("member %s\n", n.c_str());
            if (n.ends_with(".bntx"))
                for (const auto& t : BntxNames(*SarcMember(*bytes, n)))
                    std::printf("  tex %s\n", t.c_str());
        }
        for (const auto& t : BntxNames(*bytes))
            std::printf("tex %s\n", t.c_str());
        return 0;
    }
    if (op == "msbt") {
        Dictionaries dicts;
        if (const auto pack = read("/Pack/ZsDic.pack.zs"))
            dicts.Load(*pack);
        const auto raw = read(argv[3]);
        if (!raw)
            return 1;
        auto bytes = dicts.Decompress(*raw);
        if (bytes && argc >= 5)
            bytes = SarcMember(*bytes, argv[4]);
        if (!bytes)
            return 1;
        for (const auto& [label, text] : MsbtEntries(*bytes))
            std::printf("%s\t%s\n", label.c_str(), text.c_str());
        return 0;
    }
    if (op == "travel") {
        Dictionaries dicts;
        if (const auto pack = read("/Pack/ZsDic.pack.zs"))
            dicts.Load(*pack);
        constexpr const char* Kinds[]{"shrine", "lightroot", "tower"};
        for (int a = 3; a < argc; ++a) {
            const auto raw = read(argv[a]);
            const auto bytes = raw ? dicts.Decompress(*raw) : std::nullopt;
            if (!bytes) {
                std::fprintf(stderr, "unreadable: %s\n", argv[a]);
                continue;
            }
            for (const auto& p : totk::travel::Scan(*bytes))
                std::printf("%s\t%s\t%.1f\t%.1f\t%.1f\t%s\n", argv[a],
                            Kinds[static_cast<int>(p.kind)], p.x, p.y, p.z, p.state_key.c_str());
        }
        return 0;
    }
    if (op == "font" && argc >= 5) {
        const auto atlas = TotkFont::LoadGameFont(read, argv[3], 40);
        if (!atlas) {
            std::fprintf(stderr, "font failed: %s\n", argv[3]);
            return 1;
        }
        std::size_t present = 0;
        for (const auto& g : atlas->glyphs)
            present += g.advance != 0;
        std::ofstream o(argv[4], std::ios::binary);
        o << "P7\nWIDTH " << atlas->image.width << "\nHEIGHT " << atlas->image.height
          << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        o.write(reinterpret_cast<const char*>(atlas->image.rgba.data()),
                static_cast<std::streamsize>(atlas->image.rgba.size()));
        std::printf("%s: %zu glyphs, atlas %ux%u\n", argv[3], present, atlas->image.width,
                    atlas->image.height);
        return 0;
    }
    if (op == "dump" && argc >= 5) {
        astc_out = std::string(argv[4]) + ".astc";
        Decoder decoder;
        decoder.SetAstcDecoder({nullptr, &WriteAstc});
        const auto image = decoder.Load(read, argv[3]);
        if (!image) {
            std::fprintf(stderr, "decode failed: %s\n", argv[3]);
            return 1;
        }
        std::ofstream o(argv[4], std::ios::binary);
        o << "P7\nWIDTH " << image->width << "\nHEIGHT " << image->height
          << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        o.write(reinterpret_cast<const char*>(image->rgba.data()),
                static_cast<std::streamsize>(image->rgba.size()));
        std::printf("%s %ux%u\n", argv[3], image->width, image->height);
        return 0;
    }
    return 2;
}
