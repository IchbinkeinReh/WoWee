#pragma once

// Which entity a name means, of those that have it.

#include <limits>
#include <string_view>

namespace wowee::mcp {

struct PickCandidate {
    bool wholeName = false;  ///< the name asked for, and not a part of it
    bool unit = false;
    bool dead = false;
    bool lootable = false;
    float distance = 0.0f;
};

/// The smaller the better, infinite for one that is not meant: the whole name
/// before a part of one, then the nearer. `state` is "alive", "dead",
/// "lootable" or empty; with none, and where `livingFirst`, the living come
/// before the dead.
inline float pickRank(const PickCandidate& c, std::string_view state, bool livingFirst) {
    constexpr float kNever = std::numeric_limits<float>::infinity();
    if (state == "alive" && c.unit && c.dead) return kNever;
    if (state == "dead" && !(c.unit && c.dead)) return kNever;
    if (state == "lootable" && !c.lootable) return kNever;
    float rank = c.distance;
    if (!c.wholeName) rank += 1000.0f;
    if (state.empty() && livingFirst && c.unit && c.dead) rank += 500.0f;
    return rank;
}

}  // namespace wowee::mcp
