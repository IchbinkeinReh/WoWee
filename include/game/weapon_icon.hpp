#pragma once

#include <cstdint>
#include <string>

namespace wowee::game {

/// Whose picture a spell is shown with (0x0053dc20).
enum class SpellIcon { Own, MainHand, Ranged };

/// The attack's (its first effect, 78) is the weapon's; a ranged spell's that
/// repeats itself (Attributes 0x2, AttributesEx2 0x20: shoot, throw, auto shot)
/// the ranged weapon's, where that has one to show.
[[nodiscard]] constexpr SpellIcon spellIconOf(uint32_t firstEffect, uint32_t attributes, uint32_t attributesEx2) {
    if (firstEffect == 78) return SpellIcon::MainHand;
    return (attributes & 0x2u) != 0 && (attributesEx2 & 0x20u) != 0 ? SpellIcon::Ranged : SpellIcon::Own;
}

/// The attack's picture (0x005a84d0): of the form the player is in
/// (SpellShapeshiftForm's fifth column after the flags, +0x14), else of the
/// weapon in the main hand, unless it was disarmed, else that of no weapon.
[[nodiscard]] inline std::string attackIcon(const std::string& form, bool disarmed, const std::string& weapon) {
    if (!form.empty()) return form;
    return !disarmed && !weapon.empty() ? weapon : "Interface\\Buttons\\Spell-Reset";
}

/// The ranged weapon's (0x005a85e0), empty where it is disarmed (UNIT_FLAG2
/// 0x400) or one that is thrown (weapon subclass 16): then the spell's own.
[[nodiscard]] inline std::string rangedIcon(bool disarmed, uint32_t subClass, const std::string& weapon) {
    return disarmed || subClass == 16 ? std::string() : weapon;
}

}  // namespace wowee::game
