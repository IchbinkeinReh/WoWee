#pragma once

/// An heirloom's numbers at a level, as the client's item tooltip
/// (0x006277f0) and stat summary (0x0061f550) work them out of one
/// ScalingStatValues.dbc row - the row of the level shown - and the item's
/// ScalingStatValue mask:
///
/// - the stat budget (0x0061a650): one column picked by the mask's 0x4001f
///   bits; each ScalingStatDistribution stat is its modifier times the budget
///   over 10000;
/// - the armour (0x0061a6b0): one column picked by the 0xf801e0 bits;
/// - the weapon damage (0x0061a830): the damage per second of the column the
///   0x7e00 bits pick, less and plus 30% (20% for 0x400 and 0x1000), which
///   the tooltip multiplies by the swing to say a hit's range;
/// - the spell power (0x8000): column 16.
///
/// `row` is the ScalingStatValues row as its 24 int32 columns (column 0 the
/// id, 1 the level). Pure; tests/test_heirloom_scaling.cpp checks it.

#include <cstddef>
#include <cstdint>

namespace wowee::game::heirloom {

inline constexpr size_t kScalingStatValuesColumns = 24;

inline int32_t column(const int32_t* row, size_t n, size_t c) { return row && c < n ? row[c] : 0; }

/// 0x0061a650: the stat budget for the mask, 0 where no bit picks one.
inline int32_t statBudget(const int32_t* row, size_t n, uint32_t mask) {
    switch (mask & 0x4001fu) {
        case 0x1: return column(row, n, 2);
        case 0x2: return column(row, n, 3);
        case 0x4: return column(row, n, 4);
        case 0x8: return column(row, n, 17);
        case 0x10: return column(row, n, 5);
        case 0x40000: return column(row, n, 18);
        default: return 0;
    }
}

/// One distribution stat at the budget: modifier x budget / 10000, in int
/// arithmetic as the client does it.
inline int32_t scaledStat(int32_t modifier, int32_t budget) { return modifier * budget / 10000; }

/// Whether the mask scales the armour (0xf801e0), and 0x0061a6b0's value.
inline bool scalesArmour(uint32_t mask) { return (mask & 0xf801e0u) != 0; }
inline int32_t armour(const int32_t* row, size_t n, uint32_t mask) {
    switch (mask & 0xf801e0u) {
        case 0x20: return column(row, n, 6);
        case 0x40: return column(row, n, 7);
        case 0x80: return column(row, n, 8);
        case 0x100: return column(row, n, 9);
        case 0x80000: return column(row, n, 19);
        case 0x100000: return column(row, n, 20);
        case 0x200000: return column(row, n, 21);
        case 0x400000: return column(row, n, 22);
        case 0x800000: return column(row, n, 23);
        default: return 0;
    }
}

/// Whether the mask scales the damage (0x7e00).
inline bool scalesDamage(uint32_t mask) { return (mask & 0x7e00u) != 0; }

/// 0x0061a830 and the tooltip after it: one hit's least and most, the damage
/// per second's spread times the swing (seconds) when the item has one.
inline void damage(const int32_t* row, size_t n, uint32_t mask, uint32_t delayMs, float& lo, float& hi) {
    float base = 0.0f, spread = 0.3f;
    switch (mask & 0x7e00u) {
        case 0x200: base = static_cast<float>(column(row, n, 10)); break;
        case 0x400: base = static_cast<float>(column(row, n, 11)); spread = 0.2f; break;
        case 0x800: base = static_cast<float>(column(row, n, 12)); break;
        case 0x1000: base = static_cast<float>(column(row, n, 13)); spread = 0.2f; break;
        case 0x2000: base = static_cast<float>(column(row, n, 14)); break;
        case 0x4000: base = static_cast<float>(column(row, n, 15)); break;
        default: break;
    }
    lo = base - base * spread;
    hi = (spread + 1.0f) * base;
    if (delayMs != 0) {
        lo = lo * static_cast<float>(delayMs) * 0.001f;
        hi = static_cast<float>(delayMs) * hi * 0.001f;
    }
}

/// The spell power line's figure (mask 0x8000, column 16); 0 for none.
inline int32_t spellPower(const int32_t* row, size_t n, uint32_t mask) {
    return (mask & 0x8000u) != 0 ? column(row, n, 16) : 0;
}

}  // namespace wowee::game::heirloom
