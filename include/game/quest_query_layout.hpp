#pragma once

/**
 * quest_query_layout.hpp - where the strings begin in SMSG_QUEST_QUERY_RESPONSE.
 *
 * The response is a fixed block of four-byte fields followed by the quest's
 * strings: Title, Objectives, Details, EndText and - on WotLK only -
 * CompletedText. The client's reader (0x007f70e0) reads exactly that: 65 words,
 * then five strings, whatever they say. Nothing in it looks at a string to
 * decide what it is, and nothing may here: a title is "Ich arbeite... für die
 * Horde!" as soon as the server says so. Only the block's length changes
 * between expansions, so the length is the whole of knowing where the text
 * starts.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/// The number of four-byte fields before the first string on WotLK.
///
/// Counted off AzerothCore's own writer, Quest::BuildQuestData:
///
///     5  id, method, level, minLevel, zoneOrSort
///     2  type, suggestedPlayers
///     4  two reputation objectives, faction and value each
///     2  nextQuestInChain, xpId
///     1  money - one field whichever branch of the hidden-rewards test runs
///     3  moneyMaxLevel, rewSpell, rewSpellCast
///     2  honorAddition, honorMultiplier
///     7  srcItem, flags, charTitle, playersSlain, bonusTalents, arenaPoints,
///        reviewRepShowMask
///    20  four rewards and six choices, id and count each
///    15  five reward factions: id, value, override
///     4  POI continent, x, y, pointOpt
///   ----
///    65
///
/// The array sizes are QUEST_REWARDS_COUNT 4, QUEST_REWARD_CHOICES_COUNT 6 and
/// QUEST_REPUTATIONS_COUNT 5, from QuestDef.h.
constexpr size_t kWotlkQuestQueryFields = 65;

/// The byte the title starts at on WotLK.
constexpr size_t kWotlkQuestQueryStringsOffset = kWotlkQuestQueryFields * 4;

/// The same block on the earlier clients, counted off their servers' writers
/// (cmangos-tbc GossipDef.cpp, vmangos Quest.cpp; test_quest_rewards builds both
/// field for field).
constexpr size_t kTbcQuestQueryFields = 43;
constexpr size_t kClassicQuestQueryFields = 39;

/// By the quest log's stride in the player fields, which is how the packet
/// parsers tell the expansions apart (3 Classic and Turtle, 4 TBC, 5 WotLK).
constexpr size_t questQueryStringsOffset(uint8_t questLogStride) {
    if (questLogStride >= 5) return kWotlkQuestQueryStringsOffset;
    return (questLogStride == 4 ? kTbcQuestQueryFields : kClassicQuestQueryFields) * 4;
}

/// Five strings on WotLK, four before it: CompletedText is WotLK's.
constexpr int questQueryStringCount(uint8_t questLogStride) {
    return questLogStride >= 5 ? 5 : 4;
}

/// The strings as the server wrote them, in the server's order.
struct QuestQueryTexts {
    std::string title;
    std::string objectives;
    std::string description;
    std::string endText;
    std::string completedText;
    /// Every string the layout has was there and terminated.
    bool valid = false;
};

/// The strings read where the layout has them. An answer too short for all of
/// them is none.
inline QuestQueryTexts readQuestQueryTexts(const std::vector<uint8_t>& data,
                                           uint8_t questLogStride) {
    QuestQueryTexts out;
    std::string* const fields[5] = {&out.title, &out.objectives, &out.description,
                                    &out.endText, &out.completedText};
    size_t pos = questQueryStringsOffset(questLogStride);
    const int count = questQueryStringCount(questLogStride);
    for (int i = 0; i < count; ++i) {
        const size_t start = pos;
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) return {};
        fields[i]->assign(reinterpret_cast<const char*>(data.data()) + start, pos - start);
        ++pos;
    }
    out.valid = true;
    return out;
}

/// The strings put on a quest in the log, whatever it was called before: the
/// client's log has no title of its own and reads the cache record the answer
/// filled (0x0067de90).
template <class Entry>
void applyQuestQueryTexts(Entry& entry, const QuestQueryTexts& texts) {
    if (!texts.valid) return;
    entry.title = texts.title;
    entry.objectives = texts.objectives;
    entry.description = texts.description;
    entry.endText = texts.endText;
    entry.completionText = texts.completedText;
}

}  // namespace game
}  // namespace wowee
