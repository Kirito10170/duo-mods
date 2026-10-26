// SPDX-License-Identifier: GPL-3.0-or-later

// TOTK 1.4.2 GameDataMgr reader: the game's flag store, looked up by key hash the way the game
// does (main+0x646BC8). Read-only; the player actor rewrites Life and Stamina every frame, so
// writes here would not stick anyway.
//
//   mgr          = *(u64*)(main + 0x3AC46B0)
//   table(type)  = mgr + type * 0x60: +0x10 u32 count, +0x18 records*, +0x20 map*, +0x2C u32 cap
//   map entry    = {u32 hash, u32 index}; slot = hash % cap, linear probe; {0, 0} is empty
//   record       = records + index * stride (strides below), value offsets per type
//
// Keys are murmur3_32 (seed 0) of the full name, e.g. "PlayerStatus.Life"; Enum values are the
// same hash of the value name.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace totk::gmd {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

constexpr u32 Murmur3(std::string_view s, u32 seed = 0) {
    constexpr u32 c1 = 0xcc9e2d51, c2 = 0x1b873593;
    u32 h = seed;
    const std::size_t blocks = s.size() / 4;
    const auto byte = [&](std::size_t i) { return static_cast<u32>(static_cast<u8>(s[i])); };
    for (std::size_t b = 0; b < blocks; ++b) {
        u32 k = byte(b * 4) | byte(b * 4 + 1) << 8 | byte(b * 4 + 2) << 16 | byte(b * 4 + 3) << 24;
        k *= c1;
        k = k << 15 | k >> 17;
        k *= c2;
        h ^= k;
        h = h << 13 | h >> 19;
        h = h * 5 + 0xe6546b64;
    }
    u32 k = 0;
    const std::size_t t = blocks * 4;
    switch (s.size() & 3) {
    case 3:
        k ^= byte(t + 2) << 16;
        [[fallthrough]];
    case 2:
        k ^= byte(t + 1) << 8;
        [[fallthrough]];
    case 1:
        k ^= byte(t);
        k *= c1;
        k = k << 15 | k >> 17;
        k *= c2;
        h ^= k;
    }
    h ^= static_cast<u32>(s.size());
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}
static_assert(Murmur3("PlayerStatus.Life") == 0x31AB5580u);

enum class Type : u32 {
    Bool = 0,
    BoolArray = 1,
    Int = 2,
    IntArray = 3,
    Float = 4,
    FloatArray = 5,
    Enum = 6,
    EnumArray = 7,
    Vector3 = 10,
    String64Array = 17,
    UInt = 20,
};

constexpr u64 MgrSlot = 0x3AC46B0; // main-relative .bss pointer to GameDataMgr
constexpr u64 TableStride = 0x60;
constexpr u32 MaxCapacity = 1u << 20;
constexpr u32 MaxArray = 4096;

constexpr u64 Stride(Type t) {
    switch (t) {
    case Type::Bool:
        return 0x18;
    case Type::Int:
    case Type::UInt:
    case Type::Float:
        return 0x28;
    case Type::Enum:
        return 0x38;
    case Type::Vector3:
    case Type::BoolArray:
    case Type::IntArray:
    case Type::FloatArray:
    case Type::EnumArray:
        return 0x40;
    case Type::String64Array:
        return 0x90;
    }
    return 0;
}

struct Vec3 {
    float x{}, y{}, z{};
};

/// `Read` is any callable bool(u64 address, void* out, size_t size) over guest memory.
template <class Read>
class Reader {
public:
    Reader(Read read, u64 main_base) : read_{read}, main_{main_base} {}

    /// Latch the manager for one sample; false while the game has not built it yet.
    bool Begin() {
        mgr_ = 0;
        u64 mgr{};
        if (!Get(main_ + MgrSlot, mgr) || !mgr)
            return false;
        mgr_ = mgr;
        return true;
    }

    std::optional<bool> GetBool(u32 hash) {
        const auto r = Record(Type::Bool, hash);
        u8 v{};
        if (!r || !Get(*r + 0x16, v))
            return std::nullopt;
        return v != 0;
    }
    std::optional<s32> GetInt(u32 hash) { return Scalar<s32>(Type::Int, hash); }
    std::optional<float> GetFloat(u32 hash) { return Scalar<float>(Type::Float, hash); }
    /// Current value of an Enum key, as the murmur3 hash of the value name.
    std::optional<u32> GetEnum(u32 hash) {
        const auto r = Record(Type::Enum, hash);
        u32 v{};
        if (!r || !Get(*r + 0x1C, v))
            return std::nullopt;
        return v;
    }
    std::optional<Vec3> GetVec3(u32 hash) {
        const auto r = Record(Type::Vector3, hash);
        Vec3 v{};
        if (!r || !Get(*r + 0x24, v))
            return std::nullopt;
        return v;
    }
    std::optional<std::vector<s32>> GetIntArray(u32 hash) {
        const auto r = Record(Type::IntArray, hash);
        u64 count{}, data{};
        if (!r || !Get(*r + 0x20, count) || !Get(*r + 0x28, data) || count > MaxArray || !data)
            return std::nullopt;
        std::vector<s32> out(static_cast<std::size_t>(count));
        if (count && !read_(data, out.data(), out.size() * sizeof(s32)))
            return std::nullopt;
        return out;
    }
    std::optional<std::vector<std::string>> GetString64Array(u32 hash) {
        const auto r = Record(Type::String64Array, hash);
        u64 count{}, data{};
        if (!r || !Get(*r + 0x70, count) || !Get(*r + 0x78, data) || count > MaxArray || !data)
            return std::nullopt;
        constexpr std::size_t Elem = 0x50, Text = 0xC;
        std::vector<u8> raw(static_cast<std::size_t>(count) * Elem);
        if (count && !read_(data, raw.data(), raw.size()))
            return std::nullopt;
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            const char* s = reinterpret_cast<const char*>(raw.data() + i * Elem + Text);
            std::size_t n = 0;
            while (n < 64 && s[n])
                ++n;
            out.emplace_back(s, n);
        }
        return out;
    }

private:
    template <class T>
    bool Get(u64 at, T& out) {
        return read_(at, &out, sizeof(T));
    }

    template <class T>
    std::optional<T> Scalar(Type t, u32 hash) {
        const auto r = Record(t, hash);
        T v{};
        if (!r || !Get(*r + 0x1C, v))
            return std::nullopt;
        return v;
    }

    std::optional<u64> Record(Type t, u32 hash) {
        if (!mgr_)
            return std::nullopt;
        const u64 table = mgr_ + static_cast<u64>(t) * TableStride;
        u32 count{}, cap{};
        u64 records{}, map{};
        if (!Get(table + 0x10, count) || !Get(table + 0x18, records) || !Get(table + 0x20, map) ||
            !Get(table + 0x2C, cap) || !records || !map || !cap || cap > MaxCapacity)
            return std::nullopt;
        for (u32 n = 0, slot = hash % cap; n < cap; ++n, slot = (slot + 1) % cap) {
            std::array<u32, 2> e{};
            if (!Get(map + static_cast<u64>(slot) * 8, e))
                return std::nullopt;
            if (e[0] == hash) {
                if (e[1] >= count)
                    return std::nullopt;
                return records + static_cast<u64>(e[1]) * Stride(t);
            }
            if (e[0] == 0 && e[1] == 0)
                return std::nullopt;
        }
        return std::nullopt;
    }

    Read read_;
    u64 main_;
    u64 mgr_{};
};

} // namespace totk::gmd
