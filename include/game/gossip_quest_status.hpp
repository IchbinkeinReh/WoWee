#pragma once

/// What the quest icon in SMSG_GOSSIP_MESSAGE and SMSG_QUESTGIVER_QUEST_LIST
/// says about a quest, and what it does not.
///
/// The icon is the server's QuestMenu icon. A 3.3.5 server sends 2 for a quest
/// it offers and 4 for one the player has taken - finished or not: its
/// PrepareQuestMenu adds QUEST_STATUS_COMPLETE and QUEST_STATUS_INCOMPLETE
/// with the same 4. (5, 6 and 10 are taken as "can be handed in" where a
/// server does send them; 3 as taken.) So the list alone cannot say which of
/// the taken quests are done, and GossipFrame's and the greeting panel's
/// yellow "?" against grey "?" (GetGossipActiveQuests' and GetActiveTitle's
/// isComplete) has to come from the quest log, where the quest's slot carries
/// the server's complete bit - as the original client takes it.
///
/// No game headers, so a test can read it.

#include <cstdint>

namespace wowee::game::gossip_quest {

[[nodiscard]] constexpr bool iconIsAvailable(uint32_t icon) {
    return icon == 2 || icon == 7 || icon == 8;
}
/// An icon that itself says the quest can be handed in. A 3.3.5 server never
/// sends one for a gossip or greeting list (see above).
[[nodiscard]] constexpr bool iconIsCompletable(uint32_t icon) {
    return icon == 5 || icon == 6 || icon == 10;
}
[[nodiscard]] constexpr bool iconIsIncomplete(uint32_t icon) { return icon == 3 || icon == 4; }
/// Taken already, finished or not - the list GossipFrame calls active.
[[nodiscard]] constexpr bool iconIsActive(uint32_t icon) {
    return iconIsIncomplete(icon) || iconIsCompletable(icon);
}

/// Whether an active quest of the list is drawn as ready to hand in: the icon
/// says so, or the quest log has the quest and its slot says complete.
[[nodiscard]] constexpr bool activeQuestIsComplete(uint32_t icon, bool inLog, bool logComplete) {
    if (!iconIsActive(icon)) return false;
    return iconIsCompletable(icon) || (inLog && logComplete);
}

/// What the list may write back into the quest log's complete flag: only a
/// completable icon is news. 4 is sent for finished and unfinished alike, so
/// it must leave the log's own answer alone - reading it as "not complete"
/// undid the slot's complete bit every time the NPC was spoken to.
[[nodiscard]] constexpr bool logCompleteAfterList(uint32_t icon, bool logComplete) {
    return logComplete || iconIsCompletable(icon);
}

}  // namespace wowee::game::gossip_quest
