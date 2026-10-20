#pragma once

#include <cstdint>

#include "game/inventory.hpp"

namespace wowee::game {

/// Whether an item of this inventory type goes in this equipment slot,
/// counting slots from one as the interface does (1 head ... 19 tabard).
///
/// One-handed weapons go in either hand; relics, which the list names only as
/// 28, go in the ranged slot with bows, guns and thrown weapons.
inline bool inventoryTypeFitsSlot(int slot, uint32_t t) {
    namespace IT = InvType;
    switch (slot) {
        case 1:  return t == IT::HEAD;
        case 2:  return t == IT::NECK;
        case 3:  return t == IT::SHOULDERS;
        case 4:  return t == IT::SHIRT;
        case 5:  return t == IT::CHEST || t == IT::ROBE;
        case 6:  return t == IT::WAIST;
        case 7:  return t == IT::LEGS;
        case 8:  return t == IT::FEET;
        case 9:  return t == IT::WRISTS;
        case 10: return t == IT::HANDS;
        case 11: case 12: return t == IT::FINGER;     // the two rings
        case 13: case 14: return t == IT::TRINKET;    // the two trinkets
        case 15: return t == IT::BACK;
        case 16: return t == IT::ONE_HAND || t == IT::TWO_HAND || t == IT::MAIN_HAND;
        case 17: return t == IT::ONE_HAND || t == IT::SHIELD ||
                        t == IT::OFF_HAND || t == IT::HOLDABLE;
        case 18: return t == IT::RANGED_BOW || t == IT::THROWN ||
                        t == IT::RANGED_GUN || t == 28;
        case 19: return t == IT::TABARD;
        default: return false;
    }
}

}  // namespace wowee::game
