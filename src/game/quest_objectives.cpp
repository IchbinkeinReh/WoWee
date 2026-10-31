#include "game/quest_objectives.hpp"

#include "game/quest_progress.hpp"
#include "game/interface_text.hpp"
#include "game/reputation_standing.hpp"

#include <cstdlib>

namespace wowee {
namespace game {

namespace {

/// What a kill objective is about, by name: a creature for a positive id, a
/// game object for a negative one. Empty while the query is out; this asks
/// again, because a log built from a saved character can reach an objective
/// whose query was never sent.
std::string targetName(GameHandler& gh, int32_t npcOrGoId) {
    const uint32_t entry = static_cast<uint32_t>(std::abs(npcOrGoId));
    if (npcOrGoId > 0) {
        std::string name = gh.getCachedCreatureName(entry);
        if (name.empty()) gh.queryCreatureInfo(entry, 0);
        return name;
    }
    const auto* info = gh.getCachedGameObjectInfo(entry);
    if (!info || info->name.empty()) {
        gh.queryGameObjectInfo(entry, 0);
        return {};
    }
    return info->name;
}

/// One kill or object slot, written as the log writes it. Split out because the
/// world-map pin's reader asks for the same line by slot rather than by row.
QuestObjective killObjectiveLine(GameHandler& gh, const GameHandler::QuestLogEntry& q, size_t slot) {
    const auto& kill = q.killObjectives[slot];
    const uint32_t key = static_cast<uint32_t>(std::abs(kill.npcOrGoId));
    uint32_t current = 0;
    if (auto it = q.killCounts.find(key); it != q.killCounts.end()) current = it->second.first;
    const bool isObject = kill.npcOrGoId < 0;
    QuestObjective line;
    line.text = questObjectiveLine(q.objectiveTexts[slot], targetName(gh, kill.npcOrGoId), isObject,
                                   current, kill.required, interfaceString);
    line.type = isObject ? "object" : "monster";
    line.finished = current >= kill.required;
    return line;
}

QuestObjective itemObjectiveLine(GameHandler& gh, const GameHandler::QuestLogEntry& q, size_t slot) {
    const auto& item = q.itemObjectives[slot];
    uint32_t current = 0;
    if (auto it = q.itemCounts.find(item.itemId); it != q.itemCounts.end()) current = it->second;
    // By name; the number stands in while the item query is out, and the
    // answer lands with a QUEST_LOG_UPDATE behind it.
    std::string name;
    const auto* info = gh.getItemInfo(item.itemId);
    if (info && !info->name.empty()) {
        name = info->name;
    } else {
        gh.queryItemInfo(item.itemId, 0);
    }
    QuestObjective line;
    line.text = questItemObjectiveLine(name, current, item.required, interfaceString);
    line.type = "item";
    line.finished = current >= item.required;
    return line;
}

}  // namespace

bool questObjectiveSlotLine(GameHandler& gh, const GameHandler::QuestLogEntry& q, int slot,
                            QuestObjective& out) {
    // The paragraph branch, which every negative number lands in as well.
    if (slot < 0 || slot == 16 || slot == 17) {
        if (q.objectives.empty()) return false;
        out.text = q.objectives;
        out.type = "event";
        out.finished = q.complete;
        return true;
    }
    if (slot < 4) {
        // The reference hands its builder (0x005e2370) the id the slot names and
        // that builder then looks the id up from the first slot on, taking the
        // first match: a quest repeating an id answers the repeat from the
        // earlier slot, and an empty slot - id zero - matches the first empty
        // one, which is why the answer is a line with no name in it rather than
        // nothing at all.
        const size_t wanted = static_cast<size_t>(slot);
        size_t chosen = wanted;
        for (size_t i = 0; i < q.killObjectives.size() && i < wanted; ++i) {
            if (q.killObjectives[i].npcOrGoId != q.killObjectives[wanted].npcOrGoId) continue;
            chosen = i;
            break;
        }
        out = killObjectiveLine(gh, q, chosen);
        return true;
    }
    if (slot < 16) {
        // Slots 4-9 name the six required items; 10-13 the items the quest hands
        // over; and 14-15 run one array past those into their counts, so the
        // number looked up there is a wanted count standing in as an item id. All
        // of them go through one search (0x005e2630): the id has to name one of
        // the six required items, and the slot holding the quest's own item is
        // skipped unless more than one of it is wanted - which is why a
        // handed-over item answers nothing unless it happens also to be asked for.
        const uint32_t wanted =
            slot < 10 ? q.itemObjectives[slot - 4].itemId
                      : (slot < 14 ? q.objectiveSourceItems[slot - 10]
                                   : q.objectiveSourceCounts[slot - 14]);
        for (size_t i = 0; i < q.itemObjectives.size(); ++i) {
            const auto& item = q.itemObjectives[i];
            if (item.itemId != wanted) continue;
            if (item.itemId == q.sourceItemId && item.required <= 1) continue;
            out = itemObjectiveLine(gh, q, i);
            return true;
        }
        return false;
    }
    // 18 and up. This reader has no players-slain and no reputation line: those
    // exist only in the log's list.
    return false;
}

std::vector<QuestObjective> questObjectives(GameHandler& gh,
                                            const GameHandler::QuestLogEntry& q) {
    std::vector<QuestObjective> out;
    // The list opens with the quest's own objectives paragraph - the text the
    // quest giver wrote under "Objectives:", kept in the record as a 512-byte
    // buffer at +0x1a24. The walk (0x005e1460) answers it as its first row, under
    // the kind "event", and calls it finished off the quest slot's own done bit
    // rather than off any count, which is why a done quest greys the paragraph
    // out in the log instead of only the lines under it.
    if (!q.objectives.empty()) {
        QuestObjective line;
        line.text = q.objectives;
        line.type = "event";
        line.finished = q.complete;
        out.push_back(std::move(line));
    }
    for (size_t slot = 0; slot < q.killObjectives.size(); ++slot) {
        if (!isQuestObjective(q.killObjectives[slot].npcOrGoId, q.killObjectives[slot].required))
            continue;
        out.push_back(killObjectiveLine(gh, q, slot));
    }
    for (size_t slot = 0; slot < q.itemObjectives.size(); ++slot) {
        if (!isQuestObjective(static_cast<int32_t>(q.itemObjectives[slot].itemId),
                              q.itemObjectives[slot].required))
            continue;
        out.push_back(itemObjectiveLine(gh, q, slot));
    }
    // Then one line for each item the quest hands over for an objective - the
    // kill records' source items (objectiveSourceItems, the cache's +0x1c74),
    // worded QUEST_INTERMEDIATE_ITEMS_NEEDED: the name and how many are carried,
    // "Warmark Page: (2)". Only while the quest is not met - the walk gates its
    // four-slot loop on 0x005e0ea0(index, 1) answering 0, the client working out
    // for itself whether every objective is satisfied, where this reads the bit
    // the realm sets for the same thing.
    //
    // The count does not know these lines at all, and that is the reference's
    // own arithmetic rather than a gap to close; see the note in
    // game/quest_objectives.hpp. A quest with source items and a reputation
    // objective therefore loses its last line to the interface's loop, which is
    // what the client on the other end of the wire does too.
    if (!q.complete) {
        for (const uint32_t item : q.objectiveSourceItems) {
            if (item < 1) continue;
            // Carried rather than tracked: the reference asks the inventory
            // (0x00754d00(itemId, 8)) for the number, and the line is finished
            // as soon as one of them is in the bags.
            const uint32_t held = gh.countItemInBags(item);
            std::string name;
            const auto* info = gh.getItemInfo(item);
            if (info && !info->name.empty()) {
                name = info->name;
            } else {
                gh.queryItemInfo(item, 0);
                name = questUnknownName();
            }
            QuestObjective line;
            line.text = interfaceFormat("QUEST_INTERMEDIATE_ITEMS_NEEDED", "%s: (%d)",
                                        {name, std::to_string(held)});
            line.type = "item";
            line.finished = held > 0;
            out.push_back(std::move(line));
        }
    }
    // Then the players slain, or else the reputation (0x005e1460 answers
    // the one and returns).
    if (q.playersSlain > 0) {
        uint32_t current = 0;
        if (auto it = q.killCounts.find(0u); it != q.killCounts.end()) current = it->second.first;
        QuestObjective line;
        line.text = questPlayersKilledLine(current, q.playersSlain, interfaceString);
        line.type = "player";
        line.finished = current >= q.playersSlain;
        out.push_back(std::move(line));
    } else if (q.repObjectiveFaction != 0) {
        const int32_t have = gh.getFactionStanding(q.repObjectiveFaction);
        auto label = [](int32_t value) {
            const int id = reputationStandingFor(value).id;
            const std::string key = "FACTION_STANDING_LABEL" + std::to_string(id);
            return interfaceString(key.c_str(), reputationStandingFor(value).name);
        };
        QuestObjective line;
        line.text = questFactionLine(gh.getFactionNamePublic(q.repObjectiveFaction), label(have),
                                     label(q.repObjectiveValue), interfaceString);
        line.type = "reputation";
        line.finished = have >= q.repObjectiveValue;
        out.push_back(std::move(line));
    }
    return out;
}

}  // namespace game
}  // namespace wowee
