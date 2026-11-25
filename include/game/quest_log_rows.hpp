#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

/// Names as 0x0076ec80 compares them, without regard to case.
[[nodiscard]] inline bool questNameBefore(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

/// A heading's quests in the log (0x005e08f0): the lower level first - the player's
/// for one that has none of its own (-1) - and by title among equals.
struct QuestRowOrder {
    int level = 0;
    std::string title;
    [[nodiscard]] bool before(const QuestRowOrder& other) const {
        return level != other.level ? level < other.level : questNameBefore(title, other.title);
    }
};

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
