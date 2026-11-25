#pragma once

/// The reputation window's rows out of the factions the player has a standing
/// with (0x005d1880): each under the faction Faction.dbc puts it under, and
/// two groups of no faction at the end - FACTION_OTHER (0) for those under
/// none, FACTION_INACTIVE (-1) for those the player put away, which are in
/// none of the groups they would be in (0x005d0550, flag 0x20).
///
/// Pure: tests/test_reputation_standing.cpp checks it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "game/client_compare.hpp"

namespace wowee::game::reputation_tree {

/// The two groups' ids, as they are kept among the closed ones. No faction's.
inline constexpr uint32_t kOther = 0xFFFFFFFEu;
inline constexpr uint32_t kInactive = 0xFFFFFFFFu;

inline constexpr uint8_t kFlagInactive = 0x20;

/// A faction with a standing.
struct Entry {
    uint32_t factionId = 0;
    uint32_t reputationIndex = 0;
    std::string name;
    uint8_t flags = 0;
};

/// A row as it is drawn.
struct Row {
    uint32_t factionId = 0;
    /// The server's repListId, and 0 for a header the player has no
    /// standing with - such a row is drawn but cannot be acted on.
    uint32_t reputationIndex = 0;
    std::string name;
    uint8_t flags = 0;
    bool isHeader = false;    ///< something below it descends from this
    bool isChild = false;     ///< in a group that is in a group (0x005d06e0)
    bool hasRep = false;      ///< the player has a standing to show
};

/// `parentOf`, `nameOf` and `closed` are asked by faction id (nameOf of the
/// two groups as well, for their words).
template <typename ParentOf, typename NameOf, typename Closed>
std::vector<Row> rows(const std::vector<Entry>& flat, ParentOf&& parentOf, NameOf&& nameOf, Closed&& closed) {
    std::vector<Row> out;
    // Faction.dbc is data, and a cycle in it would spin here. The real chain
    // is three deep, so a small ceiling costs nothing.
    constexpr int kDeepest = 8;

    std::unordered_map<uint32_t, const Entry*> byId;
    std::unordered_set<uint32_t> groups;
    for (const Entry& e : flat) {
        byId[e.factionId] = &e;
        uint32_t p = parentOf(e.factionId);
        for (int depth = 0; p != 0 && depth < kDeepest; ++depth, p = parentOf(p)) groups.insert(p);
    }
    // A group stays where it is: 0x005d1c10 puts none of them away.
    const auto putAway = [&](const Entry& e) {
        return (e.flags & kFlagInactive) != 0 && groups.count(e.factionId) == 0;
    };

    // Everything drawn: the factions that are not put away, and what one of
    // them descends from. A group with nobody in it is not drawn, so its
    // heading is not either.
    std::unordered_set<uint32_t> drawn;
    std::vector<uint32_t> inactive;
    for (const Entry& e : flat) {
        if (putAway(e)) {
            drawn.insert(e.factionId);
            inactive.push_back(e.factionId);
            continue;
        }
        uint32_t id = e.factionId;
        for (int depth = 0; id != 0 && depth <= kDeepest; ++depth, id = parentOf(id)) drawn.insert(id);
    }
    const std::unordered_set<uint32_t> away(inactive.begin(), inactive.end());

    // The tree, with a parent outside the drawn set counting as no parent.
    std::unordered_map<uint32_t, std::vector<uint32_t>> children;
    for (const uint32_t id : drawn) {
        if (away.count(id) != 0) continue;
        const uint32_t parent = parentOf(id);
        children[drawn.count(parent) != 0 && away.count(parent) == 0 ? parent : 0u].push_back(id);
    }
    // By name everywhere, as the client compares names (0x0076ec80): the
    // groups (0x005d1710), and the factions in one (0x005d02f0), which come
    // before the groups in it - a group follows the one it is in whole, its
    // own members first.
    const auto isGroup = [&](uint32_t id) { return children.count(id) != 0; };
    const auto byName = [&](uint32_t a, uint32_t b) {
        if (isGroup(a) != isGroup(b)) return isGroup(b);
        const int order = compareCaseless(nameOf(a), nameOf(b));
        return order != 0 ? order < 0 : a < b;
    };
    std::vector<uint32_t> top;
    for (const uint32_t id : children[0]) (children.count(id) != 0 ? top : children[kOther]).push_back(id);
    children.erase(0);
    children[kInactive] = std::move(inactive);
    std::sort(top.begin(), top.end(), byName);
    for (auto& [parent, kids] : children) {
        (void)parent;
        std::sort(kids.begin(), kids.end(), byName);
    }
    // After the factions, OTHER before INACTIVE (0x005d1710).
    for (const uint32_t group : {kOther, kInactive}) {
        if (const auto it = children.find(group); it != children.end() && !it->second.empty()) top.push_back(group);
    }

    // Depth first, so a heading is immediately followed by what is under it.
    // FrameXML draws a flat list and takes the nesting from isHeader/isChild,
    // which means the order *is* the grouping: emitting in the server's order
    // and relying on the flags would file each faction under whichever heading
    // happened to be printed last.
    std::vector<std::pair<uint32_t, int>> stack;
    for (const uint32_t id : std::views::reverse(top)) stack.emplace_back(id, 0);
    while (!stack.empty()) {
        const auto [factionId, depth] = stack.back();
        stack.pop_back();
        const auto kids = children.find(factionId);
        Row row;
        row.factionId = factionId;
        row.isHeader = kids != children.end() && !kids->second.empty();
        // The group a row is in is in a group itself: a heading's is its own,
        // any other row's the one over it. FrameXML has a place for each of the
        // four - a faction right in Classic stands between Classic's heading
        // and what is in Alliance.
        row.isChild = row.isHeader ? depth > 0 : depth > 1;
        if (const auto entry = byId.find(factionId); entry != byId.end()) {
            row.reputationIndex = entry->second->reputationIndex;
            row.name = entry->second->name;
            row.flags = entry->second->flags;
            row.hasRep = true;
        } else {
            // A heading the player has no standing with - Classic and the two
            // expansion groups are the usual ones. Drawn as a bare heading.
            row.name = nameOf(factionId);
        }
        const bool isHeader = row.isHeader;
        out.push_back(std::move(row));
        // A closed group keeps its heading and loses everything under it: the
        // panel counts rows and indexes into them, so a hidden row must not be
        // in the list at all.
        if (!isHeader || closed(factionId)) continue;
        for (const uint32_t child : std::views::reverse(kids->second)) stack.emplace_back(child, depth + 1);
    }
    return out;
}

}  // namespace wowee::game::reputation_tree
