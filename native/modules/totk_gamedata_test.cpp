// SPDX-License-Identifier: GPL-3.0-or-later
// Builds a fake GameDataMgr in a byte buffer and reads it back through totk_gamedata.h.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "totk_gamedata.h"

using namespace totk::gmd;

namespace {
constexpr u64 Base = 0x1000'0000, Mgr = Base + 0x100;
std::vector<u8> mem(0x10000);
u64 next = 0x2000;

template <class T>
void Put(u64 at, const T& v) {
    std::memcpy(mem.data() + (at - Base), &v, sizeof v);
}
u64 Alloc(u64 size) {
    const u64 at = Base + next;
    next += (size + 15) & ~u64{15};
    return at;
}
// One table with a single key: returns the record address.
u64 Table(Type t, u32 hash) {
    const u64 table = Mgr + static_cast<u64>(t) * TableStride;
    const u32 count = 3, cap = 6;
    const u64 records = Alloc(Stride(t) * count), map = Alloc(cap * 8);
    Put<u32>(table + 0x10, count);
    Put<u64>(table + 0x18, records);
    Put<u64>(table + 0x20, map);
    Put<u32>(table + 0x2C, cap);
    // A colliding entry first, so the probe has to step.
    const u32 slot = hash % cap;
    Put<u32>(map + slot * 8, hash ^ 1);
    Put<u32>(map + slot * 8 + 4, 0);
    Put<u32>(map + ((slot + 1) % cap) * 8, hash);
    Put<u32>(map + ((slot + 1) % cap) * 8 + 4, 2);
    return records + 2 * Stride(t);
}
} // namespace

int main() {
    auto read = [](u64 at, void* out, std::size_t size) {
        if (at < Base || at + size > Base + mem.size())
            return false;
        std::memcpy(out, mem.data() + (at - Base), size);
        return true;
    };
    // Point main+MgrSlot into the buffer by faking main_base.
    const u64 main_base = Base + 0x80 - MgrSlot;
    Put<u64>(Base + 0x80, Mgr);

    Put<s32>(Table(Type::Int, Murmur3("PlayerStatus.Life")) + 0x1C, 120);
    Put<float>(Table(Type::Float, Murmur3("PlayerStatus.Stamina")) + 0x1C, 2560.0f);
    const u64 pos = Table(Type::Vector3, Murmur3("PlayerStatus.CurrentPos"));
    Put<float>(pos + 0x24, -274.4f);
    Put<float>(pos + 0x28, 124.8f);
    Put<float>(pos + 0x2C, -118.4f);
    const u64 arr = Table(Type::IntArray, Murmur3("Pouch.Weapon.EquipIndex"));
    const u64 data = Alloc(8);
    Put<s32>(data, 0);
    Put<s32>(data + 4, -1);
    Put<u64>(arr + 0x20, 2);
    Put<u64>(arr + 0x28, data);
    const u64 names = Table(Type::String64Array, Murmur3("Pouch.Weapon.Content.Name"));
    const u64 elems = Alloc(2 * 0x50);
    std::memcpy(mem.data() + (elems + 0xC - Base), "Weapon_Sword_052", 17);
    Put<u64>(names + 0x70, 2);
    Put<u64>(names + 0x78, elems);

    Reader r{read, main_base};
    assert(r.Begin());
    assert(r.GetInt(Murmur3("PlayerStatus.Life")) == 120);
    assert(!r.GetInt(Murmur3("PlayerStatus.MaxLife")));
    assert(r.GetFloat(Murmur3("PlayerStatus.Stamina")) == 2560.0f);
    const auto p = r.GetVec3(Murmur3("PlayerStatus.CurrentPos"));
    assert(p && p->x == -274.4f && p->z == -118.4f);
    const auto e = r.GetIntArray(Murmur3("Pouch.Weapon.EquipIndex"));
    assert(e && e->size() == 2 && (*e)[0] == 0 && (*e)[1] == -1);
    const auto n = r.GetString64Array(Murmur3("Pouch.Weapon.Content.Name"));
    assert(n && n->size() == 2 && (*n)[0] == "Weapon_Sword_052" && (*n)[1].empty());
    std::puts("totk_gamedata_test: ok");
}
