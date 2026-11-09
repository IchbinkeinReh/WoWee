// src/game/ship_path.cpp
// See include/game/ship_path.hpp.
#include "game/ship_path.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::game {

namespace {

glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                     const glm::vec3& p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

glm::vec3 catmullRomDerivative(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                               const glm::vec3& p3, float t) {
    return 0.5f * ((-p0 + p2) + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * (2.0f * t) +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * (3.0f * t * t));
}

// A segment's length as 0x004c3b10 measures it: twenty chords, at every
// twentieth of the segment's parameter (0.05 added up, as a float).
float segmentLength(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                    const glm::vec3& p3) {
    float length = 0.0f;
    glm::vec3 prev = catmullRom(p0, p1, p2, p3, 0.0f);
    float t = 0.05f;
    for (int s = 0; s < 20; ++s) {
        const glm::vec3 q = catmullRom(p0, p1, p2, p3, t);
        length += glm::length(q - prev);
        prev = q;
        t += 0.05f;
    }
    return length;
}

}  // namespace

float ShipPath::Leg::distanceTo(size_t pointIndex) const {
    // 0x004c3720: the segments before the point, the first point steering
    // only - point k ends segment k - 2.
    float d = 0.0f;
    for (size_t j = 0; j + 1 < pointIndex && j < segmentLength.size(); ++j) d += segmentLength[j];
    return d;
}

void ShipPath::Leg::sample(float fraction, glm::vec3& pos, glm::vec3& dir) const {
    if (segmentLength.empty() || length <= 0.0f) {
        pos = points.empty() ? glm::vec3(0.0f) : points[std::min<size_t>(1, points.size() - 1)];
        dir = glm::vec3(0.0f);
        return;
    }
    // 0x004c3bd0: the segment the distance falls in, and the spline's
    // parameter in it in proportion to the distance into it. The parameter
    // does not run at an even pace along a Catmull-Rom segment, so a hull
    // moving at a steady pace along its route goes faster or slower through
    // the world as a segment is longer or shorter than its neighbours - and
    // changes speed at once at a node between two of unequal length, as the
    // client's does.
    float d = std::clamp(fraction, 0.0f, 1.0f) * length;
    size_t j = 0;
    while (j + 1 < segmentLength.size() && d > segmentLength[j]) {
        d -= segmentLength[j];
        ++j;
    }
    const float t = segmentLength[j] > 0.0f ? std::clamp(d / segmentLength[j], 0.0f, 1.0f) : 0.0f;
    pos = catmullRom(points[j], points[j + 1], points[j + 2], points[j + 3], t);
    // 0x004c43b0: the way along is the chord the segment spans, unless the
    // spline's own direction there is within sixty degrees of it (their
    // cosine at least a half), when it is that.
    glm::vec3 chord = points[j + 2] - points[j + 1];
    const float chordLen2 = glm::dot(chord, chord);
    if (chordLen2 > 2.3841858e-07f) chord /= std::sqrt(chordLen2);
    dir = chord;
    const glm::vec3 tangent = catmullRomDerivative(points[j], points[j + 1], points[j + 2], points[j + 3], t);
    const float tangentLen2 = glm::dot(tangent, tangent);
    if (tangentLen2 > 0.0001f) {
        const glm::vec3 unit = tangent / std::sqrt(tangentLen2);
        if (glm::dot(unit, chord) >= 0.5f) dir = unit;
    }
}

ShipPath ShipPath::build(const std::vector<ShipPathNode>& nodes, float speed, float accel) {
    ShipPath path;
    if (!(speed > 0.0f) || !(accel > 0.0f) || nodes.size() < 2) return path;
    path.speed_ = speed;
    path.accel_ = accel;

    // 0x007f90f0: a new leg at a change of map and after a teleport node;
    // every node is a point of its leg, and a node flagged 2 that is not the
    // leg's first is a stop with its delay.
    struct RawStop { size_t index; uint32_t delayMs; };
    std::vector<std::vector<RawStop>> rawStops;
    bool teleportBefore = false;
    for (const ShipPathNode& node : nodes) {
        if (path.legs_.empty() || node.mapId != path.legs_.back().mapId || teleportBefore) {
            path.legs_.emplace_back();
            path.legs_.back().mapId = node.mapId;
            rawStops.emplace_back();
        }
        Leg& leg = path.legs_.back();
        if ((node.flags & 2u) != 0 && !leg.points.empty()) {
            rawStops.back().push_back({leg.points.size(), node.delaySeconds * 1000u});
        }
        leg.points.push_back(node.server);
        teleportBefore = (node.flags & 1u) != 0;
    }

    // The time to cover a distance from rest at full acceleration up to the
    // top speed, or the same slowing to a stop at the end of it.
    const float v = speed;
    const float a = accel;
    const float rampTime = v / a;
    const float rampDist = v * 0.5f * rampTime;
    auto oneRamp = [&](float d) {
        return d <= rampDist ? std::sqrt(2.0f * d / a) : (d - rampDist) / v + rampTime;
    };
    // 0x007f7a60: from rest to rest.
    auto twoRamps = [&](float d) {
        return d * 0.5f <= rampDist ? 2.0f * std::sqrt(d / a) : (d - 2.0f * rampDist) / v + 2.0f * rampTime;
    };
    auto ms = [](float seconds) { return static_cast<uint32_t>(std::lround(seconds * 1000.0f)); };

    // 0x007f8660: each leg's spline and its timeline.
    uint32_t pathMs = 0;
    for (size_t l = 0; l < path.legs_.size(); ++l) {
        Leg& leg = path.legs_[l];
        const size_t n = leg.points.size();
        if (n >= 4) {
            for (size_t j = 0; j + 3 < n; ++j) {
                leg.segmentLength.push_back(segmentLength(leg.points[j], leg.points[j + 1],
                                                          leg.points[j + 2], leg.points[j + 3]));
                leg.length += leg.segmentLength.back();
            }
        }
        uint32_t travelMs = 0;
        uint32_t delaysMs = 0;
        float last = 0.0f;
        size_t made = 0;
        for (const RawStop& rs : rawStops[l]) {
            if (rs.index + 1 >= n) break;   // the last point only steers
            const float at = leg.distanceTo(rs.index);
            const float d = at - last;
            travelMs += ms(made == 0 ? oneRamp(d) : twoRamps(d));
            leg.stops.push_back({travelMs + delaysMs, at, rs.delayMs});
            delaysMs += rs.delayMs;
            last = at;
            ++made;
        }
        const float d = leg.length - last;
        travelMs += ms(made == 0 ? d / v : oneRamp(d));
        leg.stops.push_back({travelMs + delaysMs, leg.length, 0u});
        leg.startMs = pathMs;
        for (Stop& s : leg.stops) s.timeMs += leg.startMs;
        pathMs = leg.stops.back().timeMs;
        leg.endMs = pathMs;
    }
    path.periodMs_ = pathMs;
    return path;
}

const ShipPath::Leg* ShipPath::legAt(uint32_t pathTimeMs) const {
    for (const Leg& leg : legs_) {
        if (pathTimeMs < leg.endMs) return &leg;
    }
    return nullptr;
}

bool ShipPath::onMapAt(uint32_t pathTimeMs, uint32_t mapId) const {
    const Leg* leg = legAt(pathTimeMs);
    return leg && leg->mapId == mapId;
}

ShipPath::Pose ShipPath::evaluate(uint32_t t) const {
    Pose pose;
    const Leg* legPtr = legAt(t);
    if (!legPtr) return pose;
    const Leg& leg = *legPtr;
    pose.mapId = leg.mapId;

    const float v = speed_;
    const float a = accel_;
    const size_t count = leg.stops.size();
    // Past which stops the time is, and whether it waits at one now.
    size_t i = 0;
    float base = 0.0f;
    uint32_t departed = leg.startMs;
    float distance = -1.0f;
    if (count != 1) {
        for (; i + 1 < count; ++i) {
            const Stop& s = leg.stops[i];
            if (t < s.timeMs) break;
            if (t - s.timeMs < s.delayMs) {
                distance = s.distance;
                pose.motion = Motion::Stopped;
                break;
            }
            base = s.distance;
            departed = s.timeMs + s.delayMs;
        }
    }
    if (distance < 0.0f) {
        const float el = static_cast<float>(t - departed) * 0.001f;
        const float span = static_cast<float>(leg.stops[i].timeMs - departed) * 0.001f;
        const bool stopAhead = i != count - 1;
        const float rampTime = v / a;
        float d;
        if (i == 0) {
            if (stopAhead) {
                // 0x007f7660: in at full speed, slowing into the stop.
                const float down = std::min(rampTime, span);
                const float cruiseEnd = span - down;
                if (el <= cruiseEnd) {
                    pose.motion = Motion::Moving;
                    d = el * v;
                } else {
                    const float u = el - cruiseEnd;
                    const float top = a * down;
                    pose.motion = Motion::Decelerating;
                    d = cruiseEnd * v + (top + top - a * u) * 0.5f * u;
                }
            } else {
                pose.motion = Motion::Moving;
                d = el * v;
            }
        } else if (stopAhead) {
            // 0x007f76f0: from a stop to the next.
            const float ramp = std::min(rampTime, span * 0.5f);
            if (span - ramp < el) {
                const float top = a * ramp;
                const float u = el - (span - ramp);
                pose.motion = Motion::Decelerating;
                d = (span - 2.0f * ramp) * v + (top * 2.0f - a * u) * 0.5f * u + top * 0.5f * ramp;
            } else if (ramp < el) {
                pose.motion = Motion::Moving;
                d = (el - rampTime) * v + v * 0.5f * rampTime;
            } else {
                pose.motion = Motion::Accelerating;
                d = el * (a * el) * 0.5f;
            }
        } else {
            // Away from the last stop to the leg's end.
            const float ramp = std::min(rampTime, span);
            if (el <= ramp) {
                pose.motion = Motion::Accelerating;
                d = a * el * 0.5f * el;
            } else {
                pose.motion = Motion::Moving;
                d = (el - rampTime) * v + v * 0.5f * rampTime;
            }
        }
        distance = d + base;
    }

    const float fraction = leg.length > 0.0f ? std::clamp(distance / leg.length, 0.0f, 1.0f) : 0.0f;
    glm::vec3 dir;
    leg.sample(fraction, pose.server, dir);
    pose.direction = glm::vec2(dir.x, dir.y);
    return pose;
}

}  // namespace wowee::game
