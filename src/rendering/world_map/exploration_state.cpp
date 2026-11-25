// exploration_state.cpp - Fog of war / exploration tracking implementation.
// Extracted from WorldMap::updateExploration, setServerExplorationMask
// (Phase 3 of refactoring plan).
#include "rendering/world_map/exploration_state.hpp"
#include "rendering/world_map/coordinate_projection.hpp"

#include <cmath>

namespace wowee {
namespace rendering {
namespace world_map {

void ExplorationState::setServerMask(const std::vector<uint32_t>& masks, bool hasData) {
    if (!hasData || masks.empty()) {
        // New session or no data yet - reset both server mask and local accumulation
        if (hasServerMask_) {
            locallyExploredZones_.clear();
        }
        hasServerMask_ = false;
        serverMask_.clear();
        return;
    }
    hasServerMask_ = true;
    serverMask_ = masks;
}

bool ExplorationState::isBitSet(uint32_t bitIndex) const {
    if (!hasServerMask_ || serverMask_.empty()) return false;
    const size_t word = bitIndex / 32;
    if (word >= serverMask_.size()) return false;
    return (serverMask_[word] & (1u << (bitIndex % 32))) != 0;
}

void ExplorationState::update(const std::vector<Zone>& zones,
                               const glm::vec3& playerRenderPos,
                               int currentZoneIdx,
                               const std::unordered_map<uint32_t, uint32_t>& exploreFlagByAreaId,
                               uint32_t playerZoneId) {
    overlaysChanged_ = false;

    if (hasServerMask_) {
        exploredZones_.clear();
        for (int i = 0; i < static_cast<int>(zones.size()); i++) {
            const auto& z = zones[i];
            if (z.areaID == 0 || z.exploreBits.empty()) continue;
            for (uint32_t bit : z.exploreBits) {
                if (isBitSet(bit)) {
                    exploredZones_.insert(i);
                    break;
                }
            }
        }
        // Also reveal the zone the player is currently standing in so the map isn't
        // pitch-black the moment they first enter a new zone.
        int curZone = findZoneForPlayer(zones, playerRenderPos, playerZoneId);
        if (curZone >= 0) exploredZones_.insert(curZone);

        // Per-overlay exploration: check each overlay's areaIDs against the exploration mask
        std::unordered_set<int> newExploredOverlays;
        if (currentZoneIdx >= 0 && currentZoneIdx < static_cast<int>(zones.size()))
            newExploredOverlays = revealed(zones[static_cast<size_t>(currentZoneIdx)], exploreFlagByAreaId);

        if (newExploredOverlays != exploredOverlays_) {
            exploredOverlays_ = std::move(newExploredOverlays);
            overlaysChanged_ = true;
        }
        return;
    }

    // Server mask unavailable - fall back to locally-accumulated position tracking.
    float wowX = playerRenderPos.y;
    float wowY = playerRenderPos.x;

    bool foundPos = false;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        const auto& z = zones[i];
        if (z.areaID == 0) continue;
        float minX = std::min(z.bounds.locLeft, z.bounds.locRight);
        float maxX = std::max(z.bounds.locLeft, z.bounds.locRight);
        float minY = std::min(z.bounds.locTop, z.bounds.locBottom);
        float maxY = std::max(z.bounds.locTop, z.bounds.locBottom);
        if (maxX - minX < 0.001f || maxY - minY < 0.001f) continue;
        if (wowX >= minX && wowX <= maxX && wowY >= minY && wowY <= maxY) {
            locallyExploredZones_.insert(i);
            foundPos = true;
        }
    }

    if (!foundPos) {
        int zoneIdx = findZoneForPlayer(zones, playerRenderPos, playerZoneId);
        if (zoneIdx >= 0) locallyExploredZones_.insert(zoneIdx);
    }

    // Display the accumulated local set
    exploredZones_ = locallyExploredZones_;

    // Without server mask, mark all overlays as explored (no fog of war)
    exploredOverlays_.clear();
    if (currentZoneIdx >= 0 && currentZoneIdx < static_cast<int>(zones.size()))
        exploredOverlays_ = revealed(zones[static_cast<size_t>(currentZoneIdx)], exploreFlagByAreaId);
}

std::unordered_set<int> ExplorationState::revealed(
    const Zone& zone, const std::unordered_map<uint32_t, uint32_t>& exploreFlagByAreaId) const {
    std::unordered_set<int> out;
    for (int oi = 0; oi < static_cast<int>(zone.overlays.size()); ++oi) {
        bool seen = !hasServerMask_;
        for (const uint32_t areaId : zone.overlays[static_cast<size_t>(oi)].areaIDs) {
            if (seen) break;
            const auto flag = areaId != 0 ? exploreFlagByAreaId.find(areaId) : exploreFlagByAreaId.end();
            seen = flag != exploreFlagByAreaId.end() && isBitSet(flag->second);
        }
        if (seen) out.insert(oi);
    }
    return out;
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
