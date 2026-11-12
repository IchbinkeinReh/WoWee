#pragma once

/// When a questgiver's mark is asked for outside its creation: an NPC that
/// becomes a questgiver after it was created - a seasonal one whose
/// UNIT_NPC_FLAGS gain the questgiver bit when its event starts. Everything
/// else is the client's own two queries: one giver as it comes into view
/// (0x006d5000), all of them at once on the quest log, money and skill
/// watchers and an abandon (0x006d50c0).
///
/// No game headers, so a test can read it.

#include <cstdint>

namespace wowee::game::quest_status_requery {

constexpr uint32_t kNpcFlagQuestGiver = 0x00000002;

/// A unit that has just become a questgiver, and so has never been asked.
[[nodiscard]] constexpr bool becameQuestGiver(uint32_t npcFlagsBefore, uint32_t npcFlagsAfter) {
    return (npcFlagsBefore & kNpcFlagQuestGiver) == 0 && (npcFlagsAfter & kNpcFlagQuestGiver) != 0;
}

}  // namespace wowee::game::quest_status_requery
