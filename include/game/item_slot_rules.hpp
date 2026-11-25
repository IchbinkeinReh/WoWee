#pragma once
// Which places of the character an item can be put in (0x007082b0), asked by
// CursorCanGoInSlot (0x005ea880 -> 0x006d4a00). Places are the player's item
// places: 0-18 worn, 19-22 the bags, the bank and its bags past them, -1 the
// ammunition.
#include <array>
#include <cstdint>

namespace wowee::game::item_slots {

/// The worn places and bag places each inventory type fits (0x00a2d288).
inline constexpr std::array<uint32_t, 29> kFits = {
    0x0,     0x1,     0x2,     0x4,     0x8,     0x10,    0x20,    0x40,    0x80,     0x100,
    0x200,   0xc00,   0x3000,  0x18000, 0x10000, 0x20000, 0x4000,  0x18000, 0x780000, 0x40000,
    0x10,    0x18000, 0x18000, 0x10000, 0x0,     0x20000, 0x20000, 0x0,     0x20000};

inline constexpr int kAmmo = -1;
inline constexpr int kMainHand = 15, kOffHand = 16, kRanged = 17, kLastBag = 22;

/// The places an inventory slot's number can name (0x005e7c00).
[[nodiscard]] constexpr bool known(int place) {
    return place == kAmmo || (place >= 0 && place <= kLastBag) || (place >= 39 && place <= 73) ||
           (place >= 86 && place <= 117);
}

/// A bag with something in it goes to the bags' places alone (0x006d4a00).
[[nodiscard]] constexpr bool bagPlace(int place) {
    return (place >= 19 && place <= kLastBag) || (place >= 67 && place <= 73);
}

struct Item {
    uint32_t itemClass = 0, subClass = 0, inventoryType = 0, flags = 0;
};

/// `relics`: the player's class wears relics (ChrClasses flag 8), and nothing
/// else in the ranged place. `gripAllows(class, subclass)`: a learnt spell lets
/// such a weapon be held so (0x006e7d24's) - asked of one of two hands or of
/// the main hand for the off hand, and of one of the off hand for the main
/// hand; never of a fist weapon nor one flagged 0x40000000.
template <typename GripAllows>
[[nodiscard]] bool fits(const Item& item, int place, bool relics, GripAllows&& gripAllows) {
    constexpr uint32_t kWeapon = 2, kProjectile = 6, kRelic = 28, kTwoHand = 17, kMain = 21, kOff = 22, kFist = 13;
    if (place == kAmmo) return item.itemClass == kProjectile;
    if (place == kRanged) {
        if (relics != (item.inventoryType == kRelic)) return false;
    } else if (item.itemClass == kWeapon &&
               ((place == kOffHand && (item.inventoryType == kTwoHand || item.inventoryType == kMain)) ||
                (place == kMainHand && item.inventoryType == kOff))) {
        if ((item.flags & 0x40000000u) != 0 || item.subClass == kFist) return false;
        return gripAllows(item.itemClass, item.subClass);
    }
    if (place > kLastBag) return true;
    return place >= 0 && item.inventoryType < kFits.size() &&
           ((kFits[item.inventoryType] >> static_cast<uint32_t>(place)) & 1u) != 0;
}

}  // namespace wowee::game::item_slots
