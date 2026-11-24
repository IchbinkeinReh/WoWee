#pragma once

// The quest log as the interface counts it: a header for every zone or kind of
// quest and the quests under it (0x005e6940 makes the list, 0x005e08f0 sorts
// it). Every quest log index there is - GetQuestLogTitle's, AddQuestWatch's,
// QUEST_WATCH_UPDATE's and QUEST_ACCEPTED's - is a place in this list.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wowee::game {

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
    const auto lower = [](const std::string& s) {
        std::string out = s;
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    std::map<int32_t, std::vector<const Entry*>> groups;
    for (const Entry& q : log) {
        if (q.questId != 0) groups[q.zoneOrSort].push_back(&q);
    }

    // Zones first and in name order, then the QuestSort groups (Epic, class,
    // profession), then those whose query has not answered.
    struct Group {
        int32_t key;
        std::string title;
        int rank;
    };
    std::vector<Group> ordered;
    for (const auto& [key, quests] : groups) {
        ordered.push_back({key, titleOf(key), key > 0 ? 0 : (key < 0 ? 1 : 2)});
    }
    std::sort(ordered.begin(), ordered.end(), [](const Group& a, const Group& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.title != b.title) return a.title < b.title;
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
            return lower(a->title) < lower(b->title);
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

}  // namespace wowee::game
