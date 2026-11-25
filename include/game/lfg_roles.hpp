#pragma once

#include <cstdint>

namespace wowee::game {

/// The dungeon finder's roles a class may take (the table by class 0x00553d60 and
/// 0x00553b50 mask with): leader 1, tank 2, healer 4, damage 8.
[[nodiscard]] constexpr uint8_t lfgClassRoles(uint8_t classId) {
    switch (classId) {
        case 1: case 6: return 0x0b;                                  // warrior, death knight
        case 2: case 11: return 0x0f;                                 // paladin, druid
        case 3: case 4: case 8: case 9: return 0x09;                  // hunter, rogue, mage, warlock
        case 5: case 7: return 0x0d;                                  // priest, shaman
        default: return 0;
    }
}

}  // namespace wowee::game
