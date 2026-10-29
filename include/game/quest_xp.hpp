#pragma once

#include <cstdint>

// The two reward-panel numbers the client works out for itself rather than being
// sent: experience (below) and money (further below). Both hang on the same level
// cap. The file is named for the first because that one needed the most room.
//
// What a quest's reward panel prints for experience (GetQuestLogRewardXP,
// 0x005e49b0). The amount is in no packet: the client reads QuestXP.dbc itself -
// the row for a level, the column the quest names - and then shapes the number in
// three steps, none of which the table does.
//
// The row is keyed on the **quest's** level, or on the player's own level when the
// quest carries -1 for "scale to me" (record +8 in 0x005e0420). A level with no row
// answers nothing, before any of the rest is considered.
//
// The damping is the WotLK rule that a quest far above or below the character pays
// less: the factor is 20 + 2*(questLevel - playerLevel), over ten tenths, clamped to
// [1, 10]. So five levels above the quest's own level is still full pay, each level
// past that costs a fifth, and ten levels past it still pays a tenth rather than
// nothing. (0x005e0420's own chain is three comparisons against 0xb and 1 that
// amount to exactly that clamp; it re-derives the same "quest level, or the
// player's" value three times, which is why the decompile looks redundant.)
//
// The rounding is the client's, not the table's. Its three tests are `< 0x65`,
// `< 0x1f5` and `< 0x3e9` - that is 101, 501 and 1001, so a hundred takes the
// nearest-5 bucket, five hundred the nearest-10 and a thousand the nearest-25 -
// and from 1001 up the nearest 50. It runs on the damped number, which is why 232
// at eight tenths answers 190: 232*8/10 is 185, and 185 rounds up. (The buckets
// happen to agree with each other at the three boundary values, so reading those
// constants as 100/500/1000 costs nothing; they are transcribed here as written.)
//
// What is left out on purpose: 0x005e49b0 also walks five quest-log slots whose
// predicate multiplies the answer by three below level 60, and multiplies it by
// whatever 0x007246a0 reads off the player's auras. Both are pinned only as far as
// "they exist"; neither is reproduced here, so a boosted or group-scaled quest can
// still read low. Related: the reward panel itself zeroes the XP line at the level
// cap (0x0058dd50 writes its XP global as zero unless the level is below
// PLAYER_FIELD_MAX_LEVEL), which is the same gate as the first rule below.
//
// Money (GetQuestLogRewardMoney, 0x005e48f0) is in the packet after all, but the
// cap changes it, and there the cap *raises* the number instead of zeroing it.
// The client takes the quest's reward amount - never the negative half of that
// field, which is a cost and belongs to GetQuestLogRequiredMoney - and when the
// player's level is not below PLAYER_FIELD_MAX_LEVEL it substitutes the quest's
// **second** money amount (record +0x38, the word right after the reward amount
// at +0x34) whenever that is at least as large as the first. Read that way the
// compare's direction makes sense: a quest that already pays well keeps its
// amount at the cap, and one that pays nothing - a grey quest, whose reward
// amount is 0 - still pays the max-level figure. At the cap a quest pays *at
// least* that much. (The second amount is what the server serializers write
// beside the first - WoWee's own count of AzerothCore's quest query block spells
// the run money(13), maxLevel(14), rewSpell(15), and the reward spell's offset
// was already checked against the panel.)

namespace wowee::game {

/// Which QuestXP.dbc row a quest reads: its own level, or the player's when the
/// quest says it scales (-1).
inline int32_t questXpRowLevel(int32_t questLevel, uint32_t playerLevel) {
    return questLevel == -1 ? static_cast<int32_t>(playerLevel) : questLevel;
}

/// The damping as tenths of the table's number: 10 is full pay.
inline int questXpDampFactor(int32_t rowLevel, uint32_t playerLevel) {
    int factor = 2 * (rowLevel - static_cast<int>(playerLevel)) + 20;
    if (factor > 10) factor = 10;
    if (factor < 1) factor = 1;
    return factor;
}

/// The client's own rounding of the reward number, after damping. It truncates
/// rather than rounding half up, which is why a number of 1 answers nothing while
/// 3 answers 5. The three thresholds are the client's own 0x65, 0x1f5 and 0x3e9.
inline uint32_t questXpRound(uint32_t xp) {
    if (xp < 101) return (xp + 2) / 5 * 5;
    if (xp < 501) return (xp + 5) / 10 * 10;
    if (xp < 1001) return (xp + 12) / 25 * 25;
    return (xp + 25) / 50 * 50;
}

/// The whole answer. `base` is the table's number for the row `rowLevel` gives and
/// the quest's difficulty column; `maxLevel` is PLAYER_FIELD_MAX_LEVEL, and zero
/// means the layout names no such field - the cap is then not applied, which keeps
/// the old answer for a realm whose field list was never read, rather than
/// pretending every character is at level 0.
inline uint32_t questRewardXp(uint32_t base, int32_t rowLevel,
                              uint32_t playerLevel, uint32_t maxLevel) {
    if (maxLevel != 0 && playerLevel >= maxLevel) return 0;
    return questXpRound(base * static_cast<uint32_t>(questXpDampFactor(rowLevel, playerLevel)) / 10u);
}

/// The money line: the quest's reward amount, or nothing when that field is a
/// cost (the negative half is GetQuestLogRequiredMoney's answer, not this one).
/// At the level cap the quest's max-level amount takes over whenever it is at
/// least as large, which is the client's `<=` - so the cap floors the payout at
/// that figure instead of replacing it. `maxLevel` is PLAYER_FIELD_MAX_LEVEL, and
/// as above zero means the layout names no cap, in which case the floor is not
/// applied; `maxLevelAmount` stays 0 for the classic and TBC layouts, whose field
/// order beside the money was never counted off a serializer, and a 0 floor can
/// only ever raise a zero payout to zero.
inline int32_t questRewardMoney(int32_t rewardMoney, int32_t maxLevelAmount,
                                uint32_t playerLevel, uint32_t maxLevel) {
    int32_t money = rewardMoney > 0 ? rewardMoney : 0;
    if (maxLevel != 0 && playerLevel >= maxLevel && money <= maxLevelAmount)
        money = maxLevelAmount;
    return money;
}

} // namespace wowee::game
