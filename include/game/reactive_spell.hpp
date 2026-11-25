#pragma once

#include <cstdint>

/// The spells that can be cast only now and then (AttributesEx4 0x200), which say so as
/// they come to it: the client's list at 0x00be8e58.
namespace wowee::game::reactive_spell {

inline constexpr uint32_t kAttribute = 0x200;

struct State {
    bool castable = false;   ///< +4, as the last rescan had it
    bool shortOf = false;    ///< +5, of the power for it
};

/// The power changed (0x0053d1b0): whether one that was castable has enough again.
[[nodiscard]] constexpr bool powerChanged(State& s, bool shortNow) {
    if (!s.castable || s.shortOf == shortNow) return false;
    s.shortOf = shortNow;
    return !shortNow;
}

/// A rescan (0x0053cf10): whether it can be cast now and could not before.
[[nodiscard]] constexpr bool rescanned(State& s, bool castable, bool shortNow) {
    const bool was = s.castable && !s.shortOf;
    s = {castable, shortNow};
    return castable && !shortNow && !was;
}

}  // namespace wowee::game::reactive_spell
