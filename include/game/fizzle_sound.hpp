#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wowee::game {

/// Resistances.dbc's FizzleSoundID by school, for each school that has a row.
using ResistanceSounds = std::array<std::optional<uint32_t>, 7>;

/// The SoundEntries row of a spell's fizzle (0x004cff30), 0 for none: that of the
/// first school of the spell's mask with a row - physical's names none.
[[nodiscard]] inline uint32_t fizzleSound(uint32_t schoolMask, const ResistanceSounds& sounds) {
    for (size_t school = 0; school < sounds.size(); ++school)
        if ((schoolMask & (1u << school)) != 0 && sounds[school]) return *sounds[school];
    return 0;
}

/// Whether the player's failed cast fizzles (0x00808200): for every reason but
/// DONT_REPORT.
[[nodiscard]] constexpr bool castFailureFizzles(uint8_t result) { return result != 0x1b; }

}  // namespace wowee::game
