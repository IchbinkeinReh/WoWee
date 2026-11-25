#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wowee::game::reputation_tree {

/// The two groups that are no faction: those in none (FACTION_OTHER, 0 in the
/// client) and the inactive ones (FACTION_INACTIVE, -1).
constexpr uint32_t kOther = 0xFFFFFFFEu, kInactive = 0xFFFFFFFFu;

struct Known {
    uint32_t id = 0;
    bool inactive = false;
};
struct Row {
    uint32_t id = 0;
    bool header = false, child = false;  ///< child: of a group in a group (0x005d06e0), its heading as well
};
/// What is asked of Faction.dbc and of the player.
struct Source {
    std::function<uint32_t(uint32_t id)> parent;
    std::function<std::string(uint32_t id)> name;
    std::function<bool(uint32_t id)> collapsed;
};

/// Without regard to case, as 0x0076ec80 compares.
[[nodiscard]] inline bool nameBefore(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

/// The reputation list (0x005d1880, 0x005d1710, 0x005d02f0): each group's heading,
/// its factions by name, then the groups in it by name; the factions in no group
/// after the groups, the inactive ones last of all. A group with nothing in it is
/// not there, one that is closed is its heading alone.
[[nodiscard]] inline std::vector<Row> rows(const std::vector<Known>& known, const Source& source) {
    std::map<uint32_t, std::vector<uint32_t>> members;  // by group, factions that are no heading
    std::map<uint32_t, std::vector<uint32_t>> groups;   // by group, the groups in it; 0 the outermost
    std::vector<uint32_t> headings;
    const auto isHeading = [&](uint32_t id) {
        return std::find(headings.begin(), headings.end(), id) != headings.end();
    };
    const auto addHeading = [&](uint32_t id) {
        // Faction.dbc's chains are three deep; a cycle in it would not end.
        for (int depth = 0; id != 0 && depth < 8 && !isHeading(id); ++depth) {
            headings.push_back(id);
            const uint32_t above = source.parent(id);
            groups[above].push_back(id);
            id = above;
        }
    };
    for (const Known& k : known)
        if (const uint32_t above = source.parent(k.id); !k.inactive && above != 0) addHeading(above);
    for (const Known& k : known) {
        if (isHeading(k.id)) continue;
        const uint32_t above = source.parent(k.id);
        members[k.inactive ? kInactive : above != 0 ? above : kOther].push_back(k.id);
    }
    const auto byName = [&](std::vector<uint32_t>& ids) {
        std::stable_sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
            return nameBefore(source.name(a), source.name(b));
        });
    };
    for (auto& [group, ids] : members) byName(ids);
    for (auto& [group, ids] : groups) byName(ids);

    std::vector<Row> out;
    const std::function<void(uint32_t, int)> put = [&](uint32_t group, int depth) {
        out.push_back({group, true, depth > 0});
        if (source.collapsed(group)) return;
        for (const uint32_t id : members[group]) out.push_back({id, false, depth > 0});
        for (const uint32_t id : groups[group]) put(id, depth + 1);
    };
    for (const uint32_t id : groups[0]) put(id, 0);
    for (const uint32_t group : {kOther, kInactive})
        if (!members[group].empty()) put(group, 0);
    return out;
}

}  // namespace wowee::game::reputation_tree
