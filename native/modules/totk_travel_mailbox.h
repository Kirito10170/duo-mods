// Mailbox layout shared with the TOTK guest helper (native/helper/totk/travel-hook.S).
// The helper runs on the game's UI thread once per frame; the module writes a request and
// the helper answers it. Offsets are bytes into the host's 4 KiB mailbox.
#pragma once

#include <cstdint>

namespace totk::mailbox {

constexpr std::uint32_t Magic = 0x000;      // helper writes MagicValue once it has run
constexpr std::uint32_t Heartbeat = 0x004;  // bumped by the helper every frame
constexpr std::uint32_t ReqSeq = 0x008;     // module: written last, hands the request over
constexpr std::uint32_t ReqOp = 0x00C;
constexpr std::uint32_t ReqEpoch = 0x010;   // must equal Epoch or the helper drops it
constexpr std::uint32_t Arg0 = 0x014;       // pouch category
constexpr std::uint32_t Arg1 = 0x018;       // content slot (add: count)
constexpr std::uint32_t ResultSeq = 0x060;  // helper: set to ReqSeq once answered
constexpr std::uint32_t ResultCode = 0x064; // 0 accepted, 1 refused, 2 bad op, 3 stale, 4 no receiver
constexpr std::uint32_t ClaimedSeq = 0x068; // helper-only: which request a caller took on
constexpr std::uint32_t Name = 0x100;       // NUL-terminated "StartPos_..." name
constexpr std::uint32_t NameSize = 256;
constexpr std::uint32_t Epoch = 4092;       // host's mailbox epoch word

constexpr std::uint32_t OpTravel = 1;
constexpr std::uint32_t OpEquip = 2;
constexpr std::uint32_t OpUnequip = 3;
constexpr std::uint32_t OpAdd = 4; // arg0 category, arg1 count, Name = actor name
constexpr std::uint32_t OpEat = 5; // arg0 category, arg1 slot
constexpr std::uint32_t MagicValue = 0x4B545654; // 'TVTK' little-endian

} // namespace totk::mailbox
