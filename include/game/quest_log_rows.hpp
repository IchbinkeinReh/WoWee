#pragma once

// The quest log as the interface counts it: a header for every zone or kind of
// quest and the quests under it (0x005e6940 makes the list, 0x005e08f0 sorts
// it). Every quest log index there is - GetQuestLogTitle's, AddQuestWatch's,
// QUEST_WATCH_UPDATE's and QUEST_ACCEPTED's - is a place in this list.

#include "game/client_compare.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wowee::game {

/// The header of quests without a zone or sort, the client's own words in every
/// language (0x005e0000).
inline constexpr const char* kQuestLogNoHeader = "Missing header! (quest designers)";

template <class Entry>
struct QuestLogRows {
    struct Row {
        bool isHeader = false;
        int32_t group = 0;             ///< the quest's or the header's zoneOrSort
        std::string headerTitle;       ///< headers only
        const Entry* quest = nullptr;  ///< quests only
    };
    std::vector<Row> rows;
    /// How many of them the log shows: the quests under a collapsed header are
    /// behind these, and have their places all the same.
    size_t visible = 0;

    /// The place of a quest, from one; 0 for one not in the log (0x005deeb0).
    [[nodiscard]] int indexOf(uint32_t questId) const {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].quest && rows[i].quest->questId == questId) return static_cast<int>(i) + 1;
        }
        return 0;
    }
};

/// `titleOf(group)` names a header; `playerLevel` stands for a level of -1.
template <class Entry, class TitleOf>
QuestLogRows<Entry> questLogRows(const std::vector<Entry>& log, const std::set<int32_t>& collapsed,
                                 TitleOf&& titleOf, int32_t playerLevel) {
    using Row = typename QuestLogRows<Entry>::Row;
    std::map<int32_t, std::vector<const Entry*>> groups;
    for (const Entry& q : log) {
        if (q.questId != 0) groups[q.zoneOrSort].push_back(&q);
    }

    // By name, zones and QuestSort groups (Epic, class, profession) alike (0x005dfdc0),
    // and those whose query has not answered at the end.
    struct Group {
        int32_t key;
        std::string title;
        int rank;
    };
    std::vector<Group> ordered;
    for (const auto& [key, quests] : groups) {
        ordered.push_back({key, titleOf(key), key != 0 ? 0 : 1});
    }
    std::sort(ordered.begin(), ordered.end(), [](const Group& a, const Group& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (const int order = compareCaseless(a.title, b.title); order != 0) return order < 0;
        return a.key < b.key;
    });

    QuestLogRows<Entry> out;
    std::vector<Row> hidden;
    for (const Group& g : ordered) {
        auto& quests = groups[g.key];
        // By level, then by title without its case.
        std::stable_sort(quests.begin(), quests.end(), [&](const Entry* a, const Entry* b) {
            const int32_t la = a->level == -1 ? playerLevel : a->level;
            const int32_t lb = b->level == -1 ? playerLevel : b->level;
            if (la != lb) return la < lb;
            return compareCaseless(a->title, b->title) < 0;
        });
        Row header;
        header.isHeader = true;
        header.group = g.key;
        header.headerTitle = g.title;
        out.rows.push_back(std::move(header));
        auto& to = collapsed.count(g.key) ? hidden : out.rows;
        for (const Entry* q : quests) {
            Row row;
            row.group = g.key;
            row.quest = q;
            to.push_back(std::move(row));
        }
    }
    out.visible = out.rows.size();
    out.rows.insert(out.rows.end(), hidden.begin(), hidden.end());
    return out;
}

/// The items a quest's button may be of (0x005e0da0), in the order they are looked
/// for on the player: the one it was begun with, then, of a quest with flag 0x20000,
/// its first four wanted.
[[nodiscard]] inline std::vector<uint32_t> questButtonItems(uint32_t sourceItem, uint32_t questFlags,
                                                            const std::vector<uint32_t>& wanted) {
    std::vector<uint32_t> items;
    if (sourceItem != 0) items.push_back(sourceItem);
    if ((questFlags & 0x20000u) == 0) return items;
    for (size_t i = 0; i < wanted.size() && i < 4; ++i)
        if (wanted[i] != 0) items.push_back(wanted[i]);
    return items;
}

}  // namespace wowee::game
