// SPDX-License-Identifier: GPL-3.0-or-later

// Minimal BYML (v2-v7) reader for TOTK's scene files (Banc/*.bcett.byml, little-endian) and its
// GameData list (big-endian): enough to walk dictionaries and arrays and read strings and
// numbers. Bounds-checked; a malformed file yields empty nodes, never a crash.

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace totk::byml {

enum class Kind : std::uint8_t {
    String = 0xA0,
    Array = 0xC0,
    Dict = 0xC1,
    StringTable = 0xC2,
    Bool = 0xD0,
    Int = 0xD1,
    Float = 0xD2,
    UInt = 0xD3,
    Int64 = 0xD4,
    UInt64 = 0xD5,
    Double = 0xD6,
    Null = 0xFF,
};

class Document;

class Node {
public:
    Node() = default;
    Node(const Document* doc, std::uint8_t type, std::uint32_t value)
        : doc_{doc}, type_{type}, value_{value} {}

    bool Valid() const { return doc_ != nullptr; }
    Kind Type() const { return static_cast<Kind>(type_); }
    bool IsDict() const { return doc_ && type_ == static_cast<std::uint8_t>(Kind::Dict); }
    bool IsArray() const { return doc_ && type_ == static_cast<std::uint8_t>(Kind::Array); }

    /// Container size (0 for scalars).
    std::uint32_t Size() const;
    /// Array element.
    Node operator[](std::uint32_t i) const;
    /// Dict value by key.
    Node operator[](std::string_view key) const;
    /// Dict entry i: key and value.
    std::pair<std::string_view, Node> Entry(std::uint32_t i) const;

    std::optional<std::string_view> String() const;
    /// Float, Int, UInt or Double as a double.
    std::optional<double> Number() const;
    /// Int, UInt, Int64 or UInt64 exactly (hashes and instance ids).
    std::optional<std::uint64_t> Integer() const;

private:
    const Document* doc_{};
    std::uint8_t type_{static_cast<std::uint8_t>(Kind::Null)};
    std::uint32_t value_{};
};

class Document {
public:
    explicit Document(std::span<const std::uint8_t> data) : data_{data} {
        if (data.size() < 16)
            return;
        if (data[0] == 'B' && data[1] == 'Y')
            big_ = true;
        else if (data[0] != 'Y' || data[1] != 'B')
            return;
        const std::uint16_t version = U16(2);
        if (version < 2 || version > 10)
            return;
        keys_ = U32(4);
        strings_ = U32(8);
        root_ = U32(12);
        ok_ = root_ != 0 && root_ < data.size();
    }

    bool Ok() const { return ok_; }
    Node Root() const {
        if (!ok_)
            return {};
        return {this, data_[root_], root_};
    }

    // ---- used by Node ----
    std::uint8_t U8(std::size_t at) const { return at < data_.size() ? data_[at] : 0; }
    std::uint16_t U16(std::size_t at) const { return static_cast<std::uint16_t>(Read(at, 2)); }
    std::uint32_t U24(std::size_t at) const { return static_cast<std::uint32_t>(Read(at, 3)); }
    std::uint32_t U32(std::size_t at) const { return static_cast<std::uint32_t>(Read(at, 4)); }
    std::uint64_t U64(std::size_t at) const { return Read(at, 8); }
    /// Entry i of the string table at `table` (key or value table).
    std::optional<std::string_view> TableString(std::uint32_t table, std::uint32_t i) const {
        if (!table || U8(table) != static_cast<std::uint8_t>(Kind::StringTable) ||
            i >= U24(table + 1))
            return std::nullopt;
        const std::size_t begin = table + U32(table + 4 + i * 4);
        const std::size_t end = table + U32(table + 8 + i * 4);
        if (begin > end || end > data_.size())
            return std::nullopt;
        std::string_view s{reinterpret_cast<const char*>(data_.data() + begin), end - begin};
        if (const auto nul = s.find('\0'); nul != std::string_view::npos)
            s = s.substr(0, nul);
        return s;
    }
    std::uint32_t KeyTable() const { return keys_; }
    std::uint32_t StringTable() const { return strings_; }

private:
    std::uint64_t Read(std::size_t at, std::size_t n) const {
        if (at + n > data_.size())
            return 0;
        std::uint64_t v{};
        for (std::size_t i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t>(data_[at + i]) << (8 * (big_ ? n - 1 - i : i));
        return v;
    }

    std::span<const std::uint8_t> data_;
    std::uint32_t keys_{}, strings_{}, root_{};
    bool ok_{}, big_{};
};

inline std::uint32_t Node::Size() const {
    if (!IsDict() && !IsArray())
        return 0;
    return doc_->U24(value_ + 1);
}

inline Node Node::operator[](std::uint32_t i) const {
    if (!IsArray() || i >= Size())
        return {};
    const std::uint32_t n = Size();
    const std::uint32_t types = value_ + 4;
    const std::uint32_t values = types + ((n + 3) & ~3u);
    const std::uint8_t t = doc_->U8(types + i);
    const std::uint32_t v = doc_->U32(values + i * 4);
    if (t == static_cast<std::uint8_t>(Kind::Array) || t == static_cast<std::uint8_t>(Kind::Dict))
        return {doc_, doc_->U8(v), v};
    return {doc_, t, v};
}

inline std::pair<std::string_view, Node> Node::Entry(std::uint32_t i) const {
    if (!IsDict() || i >= Size())
        return {};
    const std::uint32_t at = value_ + 4 + i * 8;
    const auto key = doc_->TableString(doc_->KeyTable(), doc_->U24(at)).value_or("");
    const std::uint8_t t = doc_->U8(at + 3);
    const std::uint32_t v = doc_->U32(at + 4);
    if (t == static_cast<std::uint8_t>(Kind::Array) || t == static_cast<std::uint8_t>(Kind::Dict))
        return {key, Node{doc_, doc_->U8(v), v}};
    return {key, Node{doc_, t, v}};
}

inline Node Node::operator[](std::string_view key) const {
    // Keys are sorted, but scene dicts are small: a linear scan keeps this simple.
    for (std::uint32_t i = 0, n = Size(); i < n && IsDict(); ++i)
        if (auto [k, v] = Entry(i); k == key)
            return v;
    return {};
}

inline std::optional<std::string_view> Node::String() const {
    if (!doc_ || Type() != Kind::String)
        return std::nullopt;
    return doc_->TableString(doc_->StringTable(), value_);
}

inline std::optional<double> Node::Number() const {
    if (!doc_)
        return std::nullopt;
    switch (Type()) {
    case Kind::Float: {
        float f;
        std::memcpy(&f, &value_, 4);
        return f;
    }
    case Kind::Int:
        return static_cast<std::int32_t>(value_);
    case Kind::UInt:
        return value_;
    case Kind::Double: {
        const std::uint64_t bits = doc_->U64(value_);
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    default:
        return std::nullopt;
    }
}

inline std::optional<std::uint64_t> Node::Integer() const {
    if (!doc_)
        return std::nullopt;
    switch (Type()) {
    case Kind::Int:
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value_)));
    case Kind::UInt:
        return value_;
    case Kind::Int64:
    case Kind::UInt64:
        return doc_->U64(value_);
    default:
        return std::nullopt;
    }
}

} // namespace totk::byml
