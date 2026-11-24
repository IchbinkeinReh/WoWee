#pragma once

/// What a unit's stats give it, by the client's combat game tables - the
/// character sheet's stat tooltips, and the pet's: single columns of floats, a
/// hundred levels to a class.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::game {

/// The stats the tables are asked with, as the unit's own fields have them.
struct UnitStats {
    uint8_t classId = 0;
    uint32_t level = 0;
    int agility = 0;
    int intellect = 0;
    int spirit = 0;
};

namespace stat_tables {

constexpr uint32_t kLevels = 100;
constexpr uint8_t kClasses = 11;

/// The row of a class and a level, or none.
inline bool rowOf(const UnitStats& unit, size_t& row) {
    if (unit.classId == 0 || unit.classId > kClasses || unit.level == 0) return false;
    row = static_cast<size_t>(unit.classId - 1) * kLevels + (std::min(unit.level, kLevels) - 1);
    return true;
}

/// The crit percent of a stat (0x0071bae0, 0x0071bb70): the class's base and
/// the stat times the level's ratio. A zero ratio is no crit from the stat at
/// all, the base included.
inline float critPercent(const std::vector<float>& base, const std::vector<float>& ratio, const UnitStats& unit,
                         int stat) {
    size_t row = 0;
    if (!rowOf(unit, row)) return 0.0f;
    const size_t baseRow = static_cast<size_t>(unit.classId - 1);
    if (baseRow >= base.size() || row >= ratio.size() || ratio[row] == 0.0f) return 0.0f;
    return (ratio[row] * static_cast<float>(std::max(0, stat)) + base[baseRow]) * 100.0f;
}

/// The health a second of spirit (0x0071ba60): the first 50 by gtOCTRegenHP,
/// the rest by gtRegenHPPerSpt.
inline float healthRegen(const std::vector<float>& first, const std::vector<float>& rest, const UnitStats& unit) {
    size_t row = 0;
    if (!rowOf(unit, row) || row >= first.size() || row >= rest.size()) return 0.0f;
    const float spirit = static_cast<float>(std::max(0, unit.spirit));
    const float few = std::min(spirit, 50.0f);
    return few * first[row] + (spirit - few) * rest[row];
}

/// The mana a second of spirit (0x0071b9f0).
inline float manaRegen(const std::vector<float>& perSpirit, const UnitStats& unit) {
    size_t row = 0;
    if (!rowOf(unit, row) || row >= perSpirit.size()) return 0.0f;
    return std::sqrt(static_cast<float>(std::max(0, unit.intellect))) *
           static_cast<float>(std::max(0, unit.spirit)) * perSpirit[row];
}

}  // namespace stat_tables

/// What a creature's template makes of its health and its power (0x0071b8d0,
/// 0x0071b960): 1 for a hunter's pet, else the creature cache row's (+0x34,
/// +0x38), and 0 without one.
[[nodiscard]] constexpr double templateModifier(bool huntersPet, bool known, float ofTemplate) {
    return huntersPet ? 1.0 : known ? static_cast<double>(ofTemplate) : 0.0;
}

}  // namespace wowee::game
