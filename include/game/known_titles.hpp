#pragma once

#include <array>
#include <cstdint>

// Which titles the realm says this character has earned.
//
// PLAYER_FIELD_KNOWN_TITLES is six dwords holding three 64-bit groups. The
// client reads a title id's bit out of the pair at +0x778 + (id >> 6) * 8 and
// +0x77c + (id >> 6) * 8 (IsTitleKnown, 0x0051b410), and those three pairs are
// exactly the three names its own field table gives the run -
// PLAYER__FIELD_KNOWN_TITLES, _TITLES1 and _TITLES2, at wire 626, 628 and 630,
// two dwords each. They are also how it watches the run: one eight-byte watcher
// per pair, all three raising KNOWN_TITLES_UPDATE (498) with no arguments
// (registrations at 0x006e5180).
//
// Three groups of 64 is 192 title ids, which is what GetNumTitles answers and
// what the character sheet's title dropdown loops over. Two things the client
// leaves undone are done here rather than copied: id 0 is answered not-known
// (the binding's own guard is `id == 0`), and an id past the run is answered
// not-known instead of read - the client addresses past its own six dwords and
// lands on the known-currency pair sitting at +0x790, so an id of 192 or more
// is answered out of the wrong mask entirely.

namespace wowee::game {

inline constexpr uint32_t kKnownTitleGroups = 3;                        // 64-bit groups
inline constexpr uint32_t kKnownTitleWords  = kKnownTitleGroups * 2;    // six dwords
inline constexpr uint32_t kKnownTitleCount  = kKnownTitleGroups * 64;   // 192 ids

using KnownTitleWords = std::array<uint32_t, kKnownTitleWords>;

/// Whether a title id's bit is set in the run.
///
/// The group is id / 64; inside a group the lower dword carries bit numbers
/// below 32 and the upper one the rest, which is what the client's second read
/// (at +0x77c rather than +0x778) is for.
inline constexpr bool titleIsKnown(const KnownTitleWords& words, uint32_t titleId) {
    if (titleId == 0 || titleId >= kKnownTitleCount) return false;
    const uint32_t group = titleId >> 6;
    const uint32_t bit   = titleId & 63u;
    return (words[group * 2 + (bit >> 5)] & (1u << (bit & 31u))) != 0;
}

} // namespace wowee::game
