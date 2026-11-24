#pragma once

/// Who may enter a battleground (0x0054ba00).

#include <cstdint>
#include <vector>

namespace wowee::game::battleground_rules {

inline constexpr uint32_t kRandomBattleground = 32;

/// A PvpDifficulty.dbc row.
struct Bracket {
    uint32_t map = 0, minLevel = 0, maxLevel = 0;
};

/// The random battleground at level 80 only; any other where a bracket of its
/// first map holds the level - the rows of one map lie together, and the walk
/// stops after them.
[[nodiscard]] inline bool canEnter(uint32_t typeId, uint32_t firstMap, uint32_t level,
                                   const std::vector<Bracket>& brackets) {
    if (typeId == kRandomBattleground) return level == 80;
    bool inMap = false;
    for (const auto& b : brackets) {
        if (b.map != firstMap) {
            if (inMap) break;
            continue;
        }
        inMap = true;
        if (b.minLevel <= level && level <= b.maxLevel) return true;
    }
    return false;
}

}  // namespace wowee::game::battleground_rules
