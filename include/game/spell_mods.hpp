#pragma once

#include <algorithm>
#include <cstdint>

/// The talents' and auras' spell modifiers as the client keeps and applies
/// them (Spell_C.cpp). SMSG_SET_FLAT_SPELL_MODIFIER and SMSG_SET_PCT_SPELL_
/// MODIFIER each set one value - a uint8 SpellFamilyFlags bit, a uint8 op,
/// an int32 - in a table of 96 bits by 31 ops (0x007fdc60, 0xd3c658 flat,
/// 0xd397d8 percent).
namespace wowee::game::spell_mods {

inline constexpr uint32_t kFamilyFlagBits = 96;
inline constexpr uint32_t kOpCount = 31;
/// The ops the ground-targeting circle reads.
inline constexpr uint8_t kOpRange = 5;
inline constexpr uint8_t kOpRadius = 6;
/// AttributesEx3 (+0x1c) 0x20000000: the spell takes no modifiers.
inline constexpr uint32_t kAttrEx3NoModifiers = 0x20000000u;

/// What 0x007fd970 sums for a spell and an op.
struct Sum {
    int32_t flat = 0;
    int32_t pct = 100;  ///< 100 plus the percentages, at least 0
    bool any = false;   ///< some modifier applies
};

/// 0x007fd970: a spell of the player's class's SpellFamilyName (ChrClasses
/// SpellClassSet, 0x008007a0), without AttributesEx3 0x20000000, takes the
/// flat and percent modifier of every SpellFamilyFlags bit it has.
template <class Lookup>
constexpr Sum modifiers(uint32_t spellFamily, const uint32_t familyFlags[3], uint32_t attributesEx3,
                        uint32_t classFamily, uint8_t op, Lookup&& lookup) {
    Sum s;
    if (spellFamily == 0 || spellFamily != classFamily) return s;
    if ((attributesEx3 & kAttrEx3NoModifiers) != 0) return s;
    int32_t pct = 0;
    for (uint32_t bit = 0; bit < kFamilyFlagBits; ++bit) {
        if ((familyFlags[bit >> 5] & (1u << (bit & 31))) == 0) continue;
        const auto [f, p] = lookup(static_cast<uint8_t>(bit), op);
        s.flat += f;
        pct += p;
    }
    if (s.flat == 0 && pct == 0) return s;
    s.any = true;
    s.pct = std::max(0, pct + 100);
    return s;
}

/// The value a modifier sum makes of one (0x008019c0, 0x007ff480):
/// (value + flat) x pct / 100, where any applies.
constexpr float apply(float value, const Sum& s) {
    return s.any ? (value + static_cast<float>(s.flat)) * static_cast<float>(s.pct) * 0.01f : value;
}

}  // namespace wowee::game::spell_mods
