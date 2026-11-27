#pragma once

// A quest's objective lines, the way the log and the tracker write them.
//
// "Warmaul Reaver slain: 2/5" ("getötet" on a German client), "Muck-ridden
// Core: 3/5" - the quest's EndText first (an exploration or event objective,
// where there is one), then one per kill or
// object objective, one per item, one per item the quest hands over, then the
// players slain or else the reputation, in the client's words
// (GetQuestLogLeaderBoard, 0x005e1460; quest_progress.hpp). The interface's quest log
// asks for these one at a time through GetQuestLogLeaderBoard, and the map
// window's own quest list asks for them all at once; one builder serves both,
// so the two cannot word an objective differently.

#include "game/game_handler.hpp"
#include "game/quest_progress.hpp"

#include <functional>

#include <string>
#include <vector>

namespace wowee {
namespace game {

struct QuestObjective {
    std::string text;
    /// "event", "monster", "object", "item", "player", "reputation" or "log": what
    /// GetQuestLogLeaderBoard answers as the line's kind. "object" (0x009f6f35),
    /// "item" (0x009f6f44) and "player" (0x009f6f50) are entries in one .rdata
    /// block beside "unknown"; the walk hands out "monster", "reputation" and
    /// "event" as literals of its own.
    const char* type = "monster";
    bool finished = false;
};

/// A count of none is not an objective. The server's quest template names the
/// creature an item drops from in the kill record beside it - Dextren Ward
/// carries the Hand of Dextren Ward - and writes a required count of zero on
/// that record, because there is nothing to kill. Counting those gave every
/// such quest one more line than it has, and the extra one answered "0/0",
/// which reads as finished.
inline bool isQuestObjective(int32_t id, uint32_t required) { return id != 0 && required > 0; }

/// Whether a required item has a line of its own: 0x005e1300 and 0x005e1460
/// leave out the quest's own item (+0x4c) wanted once - what the giver handed
/// over to be carried - and nothing else that names an item.
inline bool isListedItemObjective(const QuestHandler::QuestLogEntry& q,
                                  const QuestHandler::QuestLogEntry::ItemObjective& item) {
    return item.itemId != 0 && (item.itemId != q.sourceItemId || item.required > 1);
}

/// 0x005e0ea0's early no: a quest that requires nothing is never met by that
/// test, whatever its slot says.
inline bool questRequiresNothing(const QuestHandler::QuestLogEntry& q) {
    if ((q.questFlags & 6u) != 0 || q.repObjectiveFaction != 0 || q.repObjectiveFaction2 != 0 ||
        q.rewardMoney != 0 || q.playersSlain != 0)
        return false;
    for (const auto& kill : q.killObjectives) {
        if (kill.npcOrGoId != 0) return false;
    }
    for (const auto& item : q.itemObjectives) {
        if (item.itemId != 0) return false;
    }
    return true;
}

/// 0x005e10b0: nothing to do but hand it in - no kill or object, no item but the
/// quest's own one wanted once, no players slain, no exploration or event flag,
/// no reputation and no money to pay. Only once the quest's record is there
/// (0x0067de90 answers none before; level 0 is a record not yet arrived).
inline bool questHasNoObjectives(const QuestHandler::QuestLogEntry& q) {
    if (q.failed || q.level == 0) return false;
    for (const auto& kill : q.killObjectives) {
        if (kill.npcOrGoId != 0) return false;
    }
    for (const auto& item : q.itemObjectives) {
        if (isListedItemObjective(q, item)) return false;
    }
    return q.playersSlain == 0 && (q.questFlags & 6u) == 0 && q.repObjectiveFaction == 0 &&
           q.repObjectiveFaction2 == 0 && q.rewardMoney >= 0;
}

/// What the gossip and greeting lists draw as ready to hand in (GetGossipActiveQuests
/// 0x0058b490, GetActiveTitle 0x0058bcb0): the quest's log slot answers
/// 0x005e0ea0(index, 1) - met, and not a quest that requires nothing - or
/// 0x005e10b0. Read from the log alone, whatever icon the list gave it.
inline bool questReadyToHandIn(const QuestHandler::QuestLogEntry& q) {
    if (q.failed) return false;
    return (q.complete && !questRequiresNothing(q)) || questHasNoObjectives(q);
}

/// How many lines questObjectives answers, without asking for any names. Note
/// that this counts lines, not things there is to do: the client's own "does this
/// quest have any objectives" test (0x005e0ea0) looks only at the kill and item
/// ids, the players-slain count and the reputation objective, so a quest with a
/// paragraph to show and nothing countable in it answers a count of one while that
/// test says it has no objectives at all. Both readings are wanted, by different
/// callers, and only the line count is the one the interface is handed.
inline int questObjectiveCount(const GameHandler::QuestLogEntry& q) {
    // The quest's EndText is the list's first line - an exploration or event
    // objective's wording, empty for most quests: the record keeps it as a
    // 512-byte buffer (+0x1a24, `endText` here, between the 3000-byte objectives
    // and details and the completed line - not the objectives paragraph), the counting
    // walker 0x005e1300 adds one for it while it is not empty, and the filling
    // walker 0x005e1460 hands it out under the kind "event" with the quest slot's
    // own done bit as its progress. Every other line sits behind it.
    int count = q.endText.empty() ? 0 : 1;
    for (const auto& kill : q.killObjectives) {
        if (isQuestObjective(kill.npcOrGoId, kill.required)) ++count;
    }
    for (const auto& item : q.itemObjectives) {
        if (isListedItemObjective(q, item)) ++count;
    }
    // The source items questObjectives gives a line to are not counted here,
    // and that is the reference's own arithmetic rather than something to even
    // out: 0x005e1300 never looks at +0x1c74, while 0x005e1460 spends a row on
    // each of them until the quest is complete. The interface loops to the count,
    // so a quest with source items and a reputation objective loses its last
    // line - which is what the client on the other end of the wire does.
    if (q.playersSlain > 0) ++count;
    else if (q.repObjectiveFaction != 0) ++count;
    // Nothing to list and met all the same: the objectives sentence (0x005e1300).
    if (count == 0 && q.complete && !questRequiresNothing(q)) count = 1;
    return count;
}

/// One objective by its slot rather than by its place in the list, which is what
/// a world-map pin's objective index names. The reference keeps this reader
/// altogether separate (0x005e58c0, behind GetQuestPOILeaderBoard at 0x005e6650,
/// which hands the number over without the -1 GetQuestLogLeaderBoard's binding
/// applies): 0-3 the four kill slots, 4-9 the six item slots, 16 and 17 - and
/// every negative number - the quest's EndText. Slots 10-13 name the
/// items the quest hands over and 14-15 run one array past them into their counts;
/// both answer nothing, because the row builder those slots call (0x005e2630)
/// looks the id up among the quest's six required items. Players slain and the
/// reputation have no slot at all. False is the three nils the binding pushes.
bool questObjectiveSlotLine(GameHandler& gh, const GameHandler::QuestLogEntry& q, int slot,
                            QuestObjective& out);

/// Every objective the quest carries, in the log's order. A name still out on
/// its query is asked for again, and is a space until it answers, as the
/// client has it - see questObjectiveLine.
std::vector<QuestObjective> questObjectives(GameHandler& gh,
                                            const GameHandler::QuestLogEntry& quest);

/// What a quest link's tooltip needs looked up for its requirement lines:
/// names by id, how many of an item are carried, and a standing's label.
struct QuestTooltipLookup {
    std::function<std::string(int32_t npcOrGoId)> targetName;
    std::function<std::string(uint32_t itemId)> itemName;
    std::function<uint32_t(uint32_t itemId)> carried;
    std::function<std::string(uint32_t factionId)> factionName;
    std::function<std::string(int32_t value)> standingLabel;
    QuestTextSource text;
};

/// How many requirement lines a quest link's tooltip asks for (0x005e1300):
/// one for the EndText, one per kill or object slot naming anything, one per
/// item slot except the quest's own item wanted once, one for a reputation
/// objective and one for players slain. With none of those, one for a quest
/// in the log whose objectives are met (0x005e0ea0), else none. Unlike the
/// log's count, a slot needs only an id, not a count.
inline int questTooltipRequirementCount(const QuestHandler::QuestLogEntry& q, bool completeInLog) {
    int n = q.endText.empty() ? 0 : 1;
    for (const auto& kill : q.killObjectives) {
        if (kill.npcOrGoId != 0) ++n;
    }
    for (const auto& item : q.itemObjectives) {
        if (isListedItemObjective(q, item)) ++n;
    }
    if (q.repObjectiveFaction != 0) ++n;
    if (q.playersSlain != 0) ++n;
    if (n == 0 && completeInLog) n = 1;
    return n;
}

/// One requirement line of a quest link's tooltip, by its 0-based place, as
/// 0x005e1460 builds it with no progress (the tooltip's context clears the
/// progress flag, so every line is a _NOPROGRESS wording): the EndText; the
/// kill and object slots; the item slots; while the quest is not met in the
/// log, the items it hands over; then, for any later place, the players
/// slain, else the reputation, else - met in the log - the objectives text.
/// Empty when there is no line there.
inline std::string questTooltipRequirementLine(const QuestHandler::QuestLogEntry& q, int index,
                                               bool completeInLog, const QuestTooltipLookup& look) {
    auto num = [](int64_t v) { return std::to_string(v); };
    auto shown = [](const std::string& name) { return name.empty() ? questUnknownName() : name; };
    int at = -1;
    if (!q.endText.empty()) {
        at = 0;
        if (index == 0) return q.endText;
    }
    for (size_t i = 0; i < q.killObjectives.size(); ++i) {
        const auto& kill = q.killObjectives[i];
        if (kill.npcOrGoId == 0) continue;
        if (++at != index) continue;
        const std::string required = num(kill.required);
        if (!q.objectiveTexts[i].empty()) {
            return chat_channels::luaFormat(look.text("QUEST_OBJECTS_FOUND_NOPROGRESS", "%s x %d"),
                                            {q.objectiveTexts[i], required});
        }
        const std::string name = shown(look.targetName ? look.targetName(kill.npcOrGoId) : std::string());
        if (kill.npcOrGoId < 0) {
            return chat_channels::luaFormat(look.text("QUEST_OBJECTS_FOUND_NOPROGRESS", "%s x %d"),
                                            {name, required});
        }
        return chat_channels::luaFormat(look.text("QUEST_MONSTERS_KILLED_NOPROGRESS", "%s x %d"),
                                        {name, required});
    }
    for (const auto& item : q.itemObjectives) {
        if (!isListedItemObjective(q, item)) continue;
        if (++at != index) continue;
        return chat_channels::luaFormat(look.text("QUEST_ITEMS_NEEDED_NOPROGRESS", "%s x %d"),
                                        {shown(look.itemName ? look.itemName(item.itemId) : std::string()),
                                         num(item.required)});
    }
    if (!completeInLog) {
        for (const uint32_t item : q.objectiveSourceItems) {
            if (item < 1) continue;
            if (++at != index) continue;
            return chat_channels::luaFormat(
                look.text("QUEST_INTERMEDIATE_ITEMS_NEEDED", "%s: (%d)"),
                {shown(look.itemName ? look.itemName(item) : std::string()),
                 num(look.carried ? look.carried(item) : 0)});
        }
    }
    if (q.playersSlain != 0) {
        return chat_channels::luaFormat(look.text("QUEST_PLAYERS_KILLED_NOPROGRESS", "Players x %d"),
                                        {num(q.playersSlain)});
    }
    if (q.repObjectiveFaction != 0) {
        return chat_channels::luaFormat(
            look.text("QUEST_FACTION_NEEDED_NOPROGRESS", "%s:  %s"),
            {look.factionName ? look.factionName(q.repObjectiveFaction) : std::string(),
             look.standingLabel ? look.standingLabel(q.repObjectiveValue) : std::string()});
    }
    if (completeInLog) return q.objectives;
    return {};
}

/// The same lines with this client's caches behind the lookups.
std::vector<std::string> questTooltipRequirementLines(GameHandler& gh,
                                                      const GameHandler::QuestLogEntry& quest,
                                                      bool completeInLog);

}  // namespace game
}  // namespace wowee
