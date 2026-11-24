#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::game {

/// Whether a place (canonical) is in an AreaTrigger (0x006ce140): its sphere,
/// else its box, turned by its yaw about the wire's axes. Trigger: the DBC row,
/// its place canonical.
template <typename Trigger>
[[nodiscard]] bool insideAreaTrigger(const Trigger& at, float x, float y, float z) {
    const float north = x - at.x, west = y - at.y, up = z - at.z;
    if (at.radius != 0.0f) return north * north + west * west + up * up <= at.radius * at.radius;
    const float c = std::cos(-at.boxYaw), s = std::sin(-at.boxYaw);
    return std::abs(west * c - north * s) <= at.boxLength * 0.5f &&
           std::abs(west * s + north * c) <= at.boxWidth * 0.5f && std::abs(up) <= at.boxHeight * 0.5f;
}

/// AreaTriggerCheck (0x006dbe30), ten times a second: a trigger is told of as
/// the player comes into it, once, and no other while it stands in that one
/// (0x00c9d330). Nothing is held back for a time after an arrival.
class AreaTriggerWatch {
public:
    static constexpr float kIntervalSeconds = 0.1f;

    /// The trigger to tell of, null for none: the first of the map's, in the
    /// file's order, that the place is in.
    template <typename Trigger>
    const Trigger* check(const std::vector<Trigger>& triggers, uint32_t mapId, float x, float y, float z) {
        if (mapId != mapId_) {
            mapId_ = mapId;
            standsIn_ = 0;
        }
        for (const Trigger& at : triggers)
            if (standsIn_ != 0 && at.id == standsIn_ && insideAreaTrigger(at, x, y, z)) return nullptr;
        standsIn_ = 0;
        for (const Trigger& at : triggers) {
            if (at.mapId != mapId || !insideAreaTrigger(at, x, y, z)) continue;
            standsIn_ = at.id;
            return &at;
        }
        return nullptr;
    }

private:
    uint32_t mapId_ = 0xFFFFFFFFu;
    uint32_t standsIn_ = 0;
};

}  // namespace wowee::game
