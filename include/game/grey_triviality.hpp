#pragma once

/// How far below the player a quest, a creature or a nameplate has to be to
/// count as grey, and the row set the client decides that with. Arithmetic
/// only: what grey means to the caller - a greeting's quest being trivial,
/// UnitIsTrivial, a plate's level turning grey - stays with the caller.

#include <cstdint>

namespace wowee::game::triviality {

/// 0x00a12fd0, 0x00a1c4a0, 0x00a31bb8, 0x00aa34b8: the gap by band of level/5.
/// The client has **four** tables here - the quest one
/// (asked by 0x0058aba0 behind IsActiveQuestTrivial and IsAvailableQuestTrivial),
/// GetQuestGreenRange's (0x005e2860), the unit one (0x006112b0 -> 0x006de130
/// behind UnitIsTrivial) and the plate one (0x0098e5f0, the colour of a level
/// on a nameplate) - and they are four *identical* tables: read out of .rdata,
/// all twenty rows of each agree, 4, 4, 5, 5, 6, 6, 7, 7 and then eight.
///
/// So this is one function rather than four because copies written out in
/// several files drift, not because the client keeps one. It ships them
/// separately, and an expansion that made a quest grey at a different distance
/// from a mob would split this back open; that is a version difference to
/// notice, not something to keep three copies in hand for.
/// Twenty bands, level/5. Past the last one this answers 8, which is what all
/// of the callers answered before there was one function here; a level
/// 100 does not exist in this expansion, and what the client would read at a
/// twentieth row it does not have is nothing any of them depends on.
inline int greyGap(uint32_t playerLevel) {
    static constexpr int kGap[20] = {4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                     8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    const unsigned band = playerLevel / 5;
    return band < 20 ? kGap[band] : 8;
}

/// A quest grey to the player (0x0058aba0): never one of no level of its own,
/// -1.
inline bool questIsGrey(int playerLevel, int questLevel) {
    return questLevel != -1 && questLevel + greyGap(static_cast<uint32_t>(playerLevel)) < playerLevel;
}

}  // namespace wowee::game::triviality
