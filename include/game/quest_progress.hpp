#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/chat_channels.hpp"

namespace wowee::game {

// PLAYER_QUEST_LOG counter layouts differ by expansion:
// Classic packs four 6-bit counters beside the state byte, TBC stores four
// byte counters, and WotLK stores four uint16 counters across two fields.
inline std::array<uint32_t, 4> decodeQuestObjectiveCounts(
    uint8_t questLogStride, uint32_t firstWord, uint32_t secondWord = 0) {
    if (questLogStride >= 5) {
        return {
            firstWord & 0xFFFFu,
            (firstWord >> 16) & 0xFFFFu,
            secondWord & 0xFFFFu,
            (secondWord >> 16) & 0xFFFFu,
        };
    }
    if (questLogStride == 4) {
        return {
            firstWord & 0xFFu,
            (firstWord >> 8) & 0xFFu,
            (firstWord >> 16) & 0xFFu,
            (firstWord >> 24) & 0xFFu,
        };
    }
    return {
        firstWord & 0x3Fu,
        (firstWord >> 6) & 0x3Fu,
        (firstWord >> 12) & 0x3Fu,
        (firstWord >> 18) & 0x3Fu,
    };
}

inline uint8_t questObjectiveCountFieldOffset(uint8_t questLogStride) {
    // Classic combines state and counts in field 1. TBC/WotLK have a separate
    // state field and begin counters at field 2.
    return questLogStride <= 3 ? 1 : 2;
}

inline uint32_t normalizeQuestObjectiveEntry(uint32_t wireEntry) {
    // SMSG_QUESTUPDATE_ADD_KILL marks game-object entries with the high bit.
    return wireEntry & 0x7FFFFFFFu;
}

/// The quest slot's state byte, wherever the expansion keeps it.
inline uint32_t questSlotState(uint8_t questLogStride, uint32_t stateField) {
    return questLogStride <= 3 ? (stateField >> 24) & 0xFFu : stateField;
}

inline bool isQuestSlotComplete(uint8_t questLogStride, uint32_t stateField) {
    return (questSlotState(questLogStride, stateField) & 0x1u) != 0;
}

/// A timed quest whose timer ran out, or one the server failed for any other
/// reason. The bit sits beside the complete one in the same field - the
/// server's own header names them QUEST_STATE_COMPLETE = 0x0001 and
/// QUEST_STATE_FAIL = 0x0002 - so this was being read and dropped for as long
/// as completion was being read.
inline bool isQuestSlotFailed(uint8_t questLogStride, uint32_t stateField) {
    return (questSlotState(questLogStride, stateField) & 0x2u) != 0;
}

/// Where an objective line's wording comes from: a GlobalStrings key and its
/// enUS text, answered with the interface's (localized) string. The game's
/// is game::interfaceString; tests pass their own.
using QuestTextSource = std::function<std::string(const char* key, const char* enUS)>;

/// The name the client puts in a line whose creature, object or item it
/// does not know yet (0x005e1460: a single space, 0x009e1050).
inline const std::string& questUnknownName() {
    static const std::string space = " ";
    return space;
}

/// A kill or object objective's line, as GetQuestLogLeaderBoard (0x005e1460)
/// words it: the quest's own ObjectiveText for that slot where it has one, in
/// QUEST_OBJECTS_FOUND ("%s: %d/%d") whatever the target is; otherwise a game
/// object's name in QUEST_OBJECTS_FOUND and a creature's in
/// QUEST_MONSTERS_KILLED ("%s slain: %d/%d", "%s getötet: %d/%d" on a German
/// client).
inline std::string questObjectiveLine(const std::string& objectiveText, const std::string& name,
                                      bool isObject, uint32_t current, uint32_t required,
                                      const QuestTextSource& text) {
    const std::vector<std::string> counts = {std::to_string(current), std::to_string(required)};
    if (!objectiveText.empty()) {
        return chat_channels::luaFormat(text("QUEST_OBJECTS_FOUND", "%s: %d/%d"),
                                        {objectiveText, counts[0], counts[1]});
    }
    const std::string& shown = name.empty() ? questUnknownName() : name;
    if (isObject) {
        return chat_channels::luaFormat(text("QUEST_OBJECTS_FOUND", "%s: %d/%d"), {shown, counts[0], counts[1]});
    }
    return chat_channels::luaFormat(text("QUEST_MONSTERS_KILLED", "%s slain: %d/%d"), {shown, counts[0], counts[1]});
}

/// An item objective's line: QUEST_ITEMS_NEEDED with the count held, never
/// more than the count needed (0x005e1460).
inline std::string questItemObjectiveLine(const std::string& name, uint32_t held, uint32_t required,
                                          const QuestTextSource& text) {
    const uint32_t shownCount = held < required ? held : required;
    return chat_channels::luaFormat(text("QUEST_ITEMS_NEEDED", "%s: %d/%d"),
                                    {name.empty() ? questUnknownName() : name, std::to_string(shownCount),
                                     std::to_string(required)});
}

/// The players-slain objective: QUEST_PLAYERS_KILLED.
inline std::string questPlayersKilledLine(uint32_t current, uint32_t required, const QuestTextSource& text) {
    return chat_channels::luaFormat(text("QUEST_PLAYERS_KILLED", "Players slain: %d/%d"),
                                    {std::to_string(current), std::to_string(required)});
}

/// The reputation objective: QUEST_FACTION_NEEDED, the faction's name, the
/// player's standing and the one needed, each a FACTION_STANDING_LABELn.
inline std::string questFactionLine(const std::string& faction, const std::string& currentStanding,
                                    const std::string& requiredStanding, const QuestTextSource& text) {
    return chat_channels::luaFormat(text("QUEST_FACTION_NEEDED", "%s: %s / %s"),
                                    {faction, currentStanding, requiredStanding});
}

} // namespace wowee::game
