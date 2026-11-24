#pragma once

// The flight map: which nodes are on it, where, and which way a flight goes.
//
// 0x005932e0 builds the list when SMSG_SHOWTAXINODES arrives and every binding
// of the flight map reads it.

#include "game/world_map_transforms.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee::game {

/// The picture's extent, WorldMapContinent's floats at +0x24..+0x30, in the
/// wire's axes.
struct TaxiMapBounds {
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    [[nodiscard]] bool usable() const { return maxX - minX != 0.0f && maxY - minY != 0.0f; }
};

struct TaxiMapNode {
    uint32_t id = 0;
    /// Fractions of the picture, from its lower left.
    float u = 0.0f, v = 0.0f;
};

struct TaxiMap {
    /// The map the picture is of: the one the current node is shown on.
    uint32_t mapId = 0;
    TaxiMapBounds bounds;
    std::vector<TaxiMapNode> nodes;

    [[nodiscard]] const TaxiMapNode* at(int oneBased) const {
        return oneBased >= 1 && oneBased <= static_cast<int>(nodes.size())
            ? &nodes[static_cast<size_t>(oneBased - 1)] : nullptr;
    }
    [[nodiscard]] bool lists(uint32_t id) const {
        return std::any_of(nodes.begin(), nodes.end(), [id](const TaxiMapNode& n) { return n.id == id; });
    }
};

/// 0x005932e0: the known nodes shown on the map the current node is shown on,
/// by id - both taken through WorldMapTransforms - whose own map's expansion
/// the account has. None without a WorldMapContinent row for the map.
///
/// `boundsOf(mapId)` answers a TaxiMapBounds, `known(nodeId)` and
/// `mapAllowed(mapId)` bools.
template <class Nodes, class BoundsOf, class Known, class MapAllowed>
TaxiMap buildTaxiMap(const Nodes& nodes, uint32_t currentNode,
                     const std::vector<WorldMapTransform>& transforms,
                     BoundsOf&& boundsOf, Known&& known, MapAllowed&& mapAllowed) {
    TaxiMap out;
    const auto current = nodes.find(currentNode);
    if (current == nodes.end()) return out;
    out.mapId = shownPlace(transforms, current->second.mapId, current->second.x, current->second.y).mapId;
    out.bounds = boundsOf(out.mapId);
    if (!out.bounds.usable()) return out;
    const TaxiMapBounds& b = out.bounds;
    for (const auto& [id, node] : nodes) {
        if (!known(id)) continue;
        const ShownPlace at = shownPlace(transforms, node.mapId, node.x, node.y);
        if (at.mapId != out.mapId || !mapAllowed(node.mapId)) continue;
        // Each over the other axis's extent, as the client divides them; the
        // pictures are square to within three yards.
        out.nodes.push_back({id, (b.maxY - at.y) / (b.maxX - b.minX), (at.x - b.minX) / (b.maxY - b.minY)});
    }
    std::sort(out.nodes.begin(), out.nodes.end(),
              [](const TaxiMapNode& l, const TaxiMapNode& r) { return l.id < r.id; });
    return out;
}

/// One end of a leg of a route (0x00591ad0), which is divided the other way.
inline std::pair<float, float> taxiLegPos(const TaxiMap& map, const std::vector<WorldMapTransform>& transforms,
                                          uint32_t mapId, float x, float y) {
    const ShownPlace at = shownPlace(transforms, mapId, x, y);
    const TaxiMapBounds& b = map.bounds;
    if (!b.usable()) return {0.0f, 0.0f};
    return {(b.maxY - at.y) / (b.maxY - b.minY), (at.x - b.minX) / (b.maxX - b.minX)};
}

/// 0x005926c0: whether a faction's flights do not stop at a node - it has one of
/// the other side's beasts for a mount and none of one's own.
inline bool taxiNodeRefused(bool alliance, uint32_t mountA, uint32_t mountB) {
    const auto has = [&](std::initializer_list<uint32_t> set) {
        return std::any_of(set.begin(), set.end(), [&](uint32_t m) { return m == mountA || m == mountB; });
    };
    const bool horde = has({0x8b0, 0xdf6, 0x4dcd, 0x8795});
    const bool ally = has({0xefd, 0x21d, 0x6ec8, 0x6ec9, 0x7a53});
    return alliance ? (horde && !ally) : (ally && !horde);
}

/// The places of a way that are on the map its first place is on: what is flown
/// before the server takes the player to the next map. `goesOn` is whether
/// there is more of it.
template <class Point>
std::vector<Point> taxiWayOnOneMap(const std::vector<std::pair<uint32_t, Point>>& way, bool& goesOn) {
    std::vector<Point> out;
    goesOn = false;
    for (const auto& [mapId, point] : way) {
        if (mapId != way.front().first) {
            goesOn = true;
            break;
        }
        out.push_back(point);
    }
    return out;
}

struct TaxiLeg {
    uint32_t from = 0, to = 0;
    uint32_t cost = 0;
    float length = 0.0f;
};

/// The flights from the current node (0x00592bb0, 0x005926c0): over the map's
/// own nodes, a path of its own where there is one and else the stops making
/// the shortest way.
class TaxiRoutes {
public:
    void clear() {
        reached_.clear();
        direct_.clear();
        start_ = 0;
    }

    template <class Refused>
    void build(const TaxiMap& map, const std::vector<TaxiLeg>& legs, uint32_t start, Refused&& refused) {
        clear();
        start_ = start;
        reached_[start] = {};
        std::unordered_map<uint32_t, std::vector<const TaxiLeg*>> from;
        for (const TaxiLeg& leg : legs) {
            if (leg.to != start && map.lists(leg.from) && map.lists(leg.to) && !refused(leg.to))
                from[leg.from].push_back(&leg);
        }
        // Nearest first; the list is a few dozen nodes.
        std::unordered_set<uint32_t> done;
        while (true) {
            uint32_t next = 0;
            float nearest = std::numeric_limits<float>::infinity();
            for (const auto& [id, r] : reached_) {
                if (!done.count(id) && r.length < nearest) {
                    next = id;
                    nearest = r.length;
                }
            }
            if (next == 0) break;
            done.insert(next);
            const Reached here = reached_[next];
            for (const TaxiLeg* leg : from[next]) {
                const float length = here.length + leg->length;
                const auto it = reached_.find(leg->to);
                if (it != reached_.end() && !(length < it->second.length)) continue;
                reached_[leg->to] = {next, here.cost + leg->cost, length};
            }
        }
        // A path of its own is flown whatever its length (0x005914b0, 0x00590c30).
        for (const TaxiLeg* leg : from[start]) direct_.emplace(leg->to, leg->cost);
    }

    [[nodiscard]] bool reaches(uint32_t id) const { return id != start_ && reached_.count(id) != 0; }
    [[nodiscard]] uint32_t cost(uint32_t id) const {
        if (const auto d = direct_.find(id); d != direct_.end()) return d->second;
        const auto it = reached_.find(id);
        return it == reached_.end() ? 0 : it->second.cost;
    }
    /// The nodes from the current one to `id`, both included; empty without a way.
    [[nodiscard]] std::vector<uint32_t> to(uint32_t id) const {
        if (!reaches(id)) return {};
        if (direct_.count(id)) return {start_, id};
        std::vector<uint32_t> route;
        for (uint32_t at = id; at != start_; at = reached_.at(at).prev) route.push_back(at);
        route.push_back(start_);
        std::reverse(route.begin(), route.end());
        return route;
    }

private:
    struct Reached {
        uint32_t prev = 0;
        uint32_t cost = 0;
        float length = 0.0f;
    };
    std::unordered_map<uint32_t, Reached> reached_;
    /// The fares of the paths leading from the current node itself.
    std::unordered_map<uint32_t, uint32_t> direct_;
    uint32_t start_ = 0;
};

}  // namespace wowee::game
