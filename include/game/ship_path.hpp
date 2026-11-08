// include/game/ship_path.hpp
// A ship's or a zeppelin's route as the original client flies it.
//
// A moving transport (game object type 15) takes its route from its taxi path
// (TaxiPathNode.dbc) and its moveSpeed and accelRate (the template's data1 and
// data2, read by 0x00711b50). 0x007f90f0 cuts the path into legs - a new one at
// each change of map and after each node flagged as a teleport (flag 1) - and
// collects the stops of each leg: the nodes flagged 2, other than its first,
// with their delays. 0x007f8660 lays one Catmull-Rom spline through each leg's
// nodes, the first and the last of them only steering it, and times the leg:
// from its start at full speed, slowing into its first stop; from each stop,
// speeding up and slowing into the next; from the last stop, speeding up to
// the leg's end. 0x007f82b0 then gives the place at a path time: the distance
// along the leg from that profile, the spline at that share of the leg's
// length, and the facing from the spline's direction there, reversed.
//
// So a hull eases into its dock and out of it, and its heading at the stop is
// the line of the route through it - not a heading of its own for the wait.
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace wowee::game {

struct ShipPathNode {
    glm::vec3 server{0.0f};   ///< TaxiPathNode's place, server axes
    uint32_t mapId = 0;
    uint32_t flags = 0;       ///< 1 teleport after this node, 2 stop here
    uint32_t delaySeconds = 0;
};

class ShipPath {
public:
    enum class Motion : uint8_t { Stopped, Accelerating, Moving, Decelerating };

    struct Pose {
        uint32_t mapId = 0;
        glm::vec3 server{0.0f};   ///< server axes
        glm::vec2 direction{0.0f};///< the spline's direction there, server x/y
        Motion motion = Motion::Stopped;
    };

    /// 0x007f90f0 / 0x007f8660. Empty when speed or acceleration is not
    /// positive or no leg has a stretch to fly.
    static ShipPath build(const std::vector<ShipPathNode>& nodes, float speed, float accel);

    [[nodiscard]] bool valid() const { return periodMs_ > 0 && !legs_.empty(); }
    [[nodiscard]] uint32_t periodMs() const { return periodMs_; }

    /// 0x007f82b0, for a time already taken modulo the period.
    [[nodiscard]] Pose evaluate(uint32_t pathTimeMs) const;

    /// Whether the hull is on this map at this path time.
    [[nodiscard]] bool onMapAt(uint32_t pathTimeMs, uint32_t mapId) const;

private:
    struct Stop {
        uint32_t timeMs = 0;   ///< arrival, path time
        float distance = 0.0f; ///< along the leg
        uint32_t delayMs = 0;
    };
    struct Leg {
        uint32_t mapId = 0;
        std::vector<glm::vec3> points;   ///< server axes, the ends steering only
        std::vector<float> segmentLength;
        float length = 0.0f;
        uint32_t startMs = 0;
        uint32_t endMs = 0;
        std::vector<Stop> stops;         ///< the leg's stops, then its end
        [[nodiscard]] float distanceTo(size_t pointIndex) const;
        void sample(float fraction, glm::vec3& pos, glm::vec3& dir) const;
    };

    const Leg* legAt(uint32_t pathTimeMs) const;

    std::vector<Leg> legs_;
    float speed_ = 0.0f;
    float accel_ = 0.0f;
    uint32_t periodMs_ = 0;
};

}  // namespace wowee::game
