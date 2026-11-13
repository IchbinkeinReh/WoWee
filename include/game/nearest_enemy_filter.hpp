#pragma once

// Which units TargetNearestEnemy (and Tab) may pick: the client's filter
// 0x00518e40, case 1 (enemies) and case 2 (enemy players).
//
//   1. 0x00729740: the player may attack it.
//   2. Not dead by its vtable +0x128: for CGUnit_C (vtable 0x00a34d90) that is
//      0x00716490, health <= 0 (descriptor +0x48, UNIT_FIELD_HEALTH).
//   3. Not flagged dead: descriptor +0x124 bit 5 - the descriptor starts at
//      UNIT_FIELD_CHARM (index 6), so +0x124 is index 79, UNIT_DYNAMIC_FLAGS,
//      and bit 5 is UNIT_DYNFLAG_DEAD (0x20).
//   4. Not lying dead: vtable +0x138 is the stand state - for CGUnit_C
//      0x0071a380 reads byte 0 of descriptor +0x110 (index 74,
//      UNIT_FIELD_BYTES_1); the active player's own (0x006d64e0, vtable
//      0x00a326c8) reads its local copy - and 7 is UNIT_STAND_STATE_DEAD.

#include <cstdint>

namespace wowee::game {

inline constexpr uint32_t kNearestEnemyDynFlagDead = 0x20;   // UNIT_DYNFLAG_DEAD
inline constexpr uint8_t  kUnitStandStateDead      = 7;      // UNIT_STAND_STATE_DEAD

[[nodiscard]] constexpr bool nearestEnemyCandidate(bool mayAttack, uint32_t health,
                                                   uint32_t dynamicFlags, uint8_t standState) {
    return mayAttack && health > 0 && (dynamicFlags & kNearestEnemyDynFlagDead) == 0 &&
           standState != kUnitStandStateDead;
}

}  // namespace wowee::game
