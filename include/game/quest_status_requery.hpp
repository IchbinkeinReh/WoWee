#pragma once

/// When the marks over questgivers have to be asked for again.
///
/// The server answers CMSG_QUESTGIVER_STATUS_QUERY for the player as it is
/// at that moment, and never says it again by itself. An NPC of the other
/// faction is answered "nothing" (the server shows no quest status to an
/// enemy), so a Horde game master who arrived in Goldshire before `.gm on`
/// kept the empty answer: `.gm on` makes the player friendly to everyone
/// (UNIT_FIELD_FACTIONTEMPLATE and PLAYER_FLAGS_GM change) and the server
/// would now say "available", but nothing asked. The same holds for an NPC
/// that becomes a questgiver after it was created - a seasonal one whose
/// UNIT_NPC_FLAGS gain the questgiver bit when its event starts: the status
/// is asked for only when a questgiver is created.
///
/// No game headers, so a test can read it.

#include <cstdint>

namespace wowee::game::quest_status_requery {

constexpr uint32_t kNpcFlagQuestGiver = 0x00000002;
constexpr uint32_t kPlayerFlagGm = 0x00000008;

/// The player's own faction template changed: how every NPC regards them,
/// and so what the server answers about their quests.
[[nodiscard]] constexpr bool playerFactionChanged(uint32_t before, uint32_t after) {
    return before != after;
}

/// `.gm on` / `.gm off`.
[[nodiscard]] constexpr bool gmFlagChanged(uint32_t flagsBefore, uint32_t flagsAfter) {
    return ((flagsBefore ^ flagsAfter) & kPlayerFlagGm) != 0;
}

/// A unit that has just become a questgiver, and so has never been asked.
[[nodiscard]] constexpr bool becameQuestGiver(uint32_t npcFlagsBefore, uint32_t npcFlagsAfter) {
    return (npcFlagsBefore & kNpcFlagQuestGiver) == 0 && (npcFlagsAfter & kNpcFlagQuestGiver) != 0;
}

}  // namespace wowee::game::quest_status_requery
