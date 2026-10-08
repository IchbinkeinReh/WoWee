#pragma once

/// The PvP flags 0x00729740 reads to decide whether two players may fight.

#include <cstdint>

namespace wowee::game {

/// The flags as WotLK keeps them, byte 1 of UNIT_FIELD_BYTES_2.
namespace pvp {
constexpr uint32_t kPvp = 1;
constexpr uint32_t kContested = 2;
constexpr uint32_t kFreeForAll = 4;
constexpr uint32_t kSanctuary = 8;
}  // namespace pvp

/// UNIT_FIELD_FLAGS 0x1000, the unit flagged for PvP.
constexpr uint32_t kUnitFlagPvp = 0x00001000;

/// A unit's PvP flags. In WotLK they are byte 1 of UNIT_FIELD_BYTES_2. In
/// Classic and TBC that field's byte 0 is the sheath state as in WotLK, but
/// byte 1 holds other flags (UNIT_BYTE2_FLAG_*: the aura display and
/// unknowns), so the PvP flag comes from UNIT_FIELD_FLAGS instead.
constexpr uint32_t unitPvpFlags(bool bytes2HoldsPvpFlags, uint32_t bytes2, uint32_t unitFlags) {
    if (bytes2HoldsPvpFlags) return (bytes2 >> 8) & 0xFFu;
    return (unitFlags & kUnitFlagPvp) != 0 ? pvp::kPvp : 0u;
}

/// 0x00729740's reading of both sides' flags: the target flagged for PvP,
/// or either side contested, and neither in a sanctuary - or both in
/// free-for-all.
constexpr bool pvpFlagsAllowAttack(uint32_t mine, uint32_t theirs) {
    if ((theirs & pvp::kPvp) == 0) {
        if ((mine & pvp::kFreeForAll) != 0 && (theirs & pvp::kFreeForAll) != 0) return true;
        if ((mine & pvp::kContested) == 0 && (theirs & pvp::kContested) == 0) return false;
    }
    if ((mine & pvp::kSanctuary) != 0) return false;
    return (theirs & pvp::kSanctuary) == 0;
}

}  // namespace wowee::game
