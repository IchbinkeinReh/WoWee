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

#include <string>
#include <vector>

namespace wowee {
namespace game {

struct QuestObjective {
    std::string text;
    /// "event", "monster", "object", "item", "player" or "reputation": what
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
        if (isQuestObjective(static_cast<int32_t>(item.itemId), item.required)) ++count;
    }
    // The source items questObjectives gives a line to are not counted here,
    // and that is the reference's own arithmetic rather than something to even
    // out: 0x005e1300 never looks at +0x1c74, while 0x005e1460 spends a row on
    // each of them until the quest is complete. The interface loops to the count,
    // so a quest with source items and a reputation objective loses its last
    // line - which is what the client on the other end of the wire does.
    if (q.playersSlain > 0) ++count;
    else if (q.repObjectiveFaction != 0) ++count;
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

}  // namespace game
}  // namespace wowee
