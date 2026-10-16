#pragma once

/// Which baked colours a building's own lights shade with their traced
/// shadow, for the ray traced lighting's baked shade (rt_trace.comp.glsl,
/// docs/deferred_rt_design.md "A building's lights keep to their storey").
///
/// A building's baked colour - its interior's vertex colours (MOCV), its
/// doodads' MODD colour - already holds its lamps' light and their shadows
/// as the bake had them. With the tracing on, a light baked into it (a MOLT
/// light, an interior doodad's lamp) gives it no light, only the share of it
/// that its traced shadow blocks (rtBakedDarkness). Traced from every baked
/// surface in reach, the Goldshire inn's hall lights - its hearth, the lamps
/// hung a yard under the hall's ceiling, its lanterns - darkened the rooms
/// upstairs: their walls face those lights sideways through the floorboards,
/// so the trace's "behind the face" test keeps them in, and every ray meets
/// the floor. Those lights never lit those colours.
///
/// So a building light shades only the baked colours of its own storey: from
/// the floor straight below it (the first face turned upward a ray down from
/// it meets) to the first face straight above it (its ceiling). Rooms side by
/// side on one floor still share their lamps, as the bake has them share
/// their light through the doors; what lies under the floor or over the
/// ceiling is left as painted.
///
/// The file's own link from a light to its groups (MOGP flag 0x200, MOLR) is
/// no guide to this. The client only places each group's MOLR lights with the
/// instance (0x007b64f0) and lights nothing with them at runtime; the inn
/// lists all nine of its hall's and rooms' lights under its exterior shell
/// (group 4) and none under its rooms, and a corridor upstairs with no lamp
/// of its own is painted with the light of the rooms beside it.
///
/// Pure: tests/test_rt_light_storey.cpp checks it.

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::rt_light_storey {

/// A face counts as a floor when its normal leans no more than 60 degrees
/// from straight up: a floorboard, a step, a hearth's stone, a ramp.
inline constexpr float kFloorMinUp = 0.5f;
/// How far down a light's floor and up its ceiling are looked for, in the
/// model's yards: a lamp in a hall hangs eight yards over its stairs (the
/// Goldshire inn's).
inline constexpr float kMaxReach = 30.0f;
/// A floor this little above the light still counts (a light on its floor).
inline constexpr float kFloorAbove = 0.05f;
/// How far outside its storey a surface may lie and still be shaded: the
/// floor's own boards and the ceiling's are of it (rt_trace.comp.glsl's
/// RT_STOREY_MARGIN).
inline constexpr float kMargin = 0.25f;

/// One group of a building, in the model's space.
struct GroupMesh {
    const std::vector<glm::vec3>* vertices = nullptr;
    const std::vector<uint16_t>* indices = nullptr;
    /// Its vertices' box, if known: a group the light is not over or under,
    /// or out of reach, is passed by whole (a city has thousands of groups).
    bool hasBounds = false;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

/// A light's storey, in the space it was found in: from its floor to its
/// ceiling (+infinity when it has none within kMaxReach).
struct Storey {
    float floorZ = 0.0f;
    float ceilingZ = 0.0f;
};

namespace detail {
// Calls f(z, up) for every face that the vertical line through p crosses,
// within kMaxReach of p: the face's height there and its normal's z over its
// length (1 straight up, -1 straight down).
template <class F>
void facesOnVertical(const glm::vec3& p, const std::vector<GroupMesh>& groups, F&& f) {
    for (const GroupMesh& g : groups) {
        if (!g.vertices || !g.indices) continue;
        if (g.hasBounds && (p.x < g.boundsMin.x || p.x > g.boundsMax.x || p.y < g.boundsMin.y ||
                            p.y > g.boundsMax.y || g.boundsMin.z > p.z + kMaxReach ||
                            g.boundsMax.z < p.z - kMaxReach)) {
            continue;
        }
        const auto& v = *g.vertices;
        const auto& idx = *g.indices;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            if (idx[i] >= v.size() || idx[i + 1] >= v.size() || idx[i + 2] >= v.size()) continue;
            const glm::vec3& a = v[idx[i]];
            const glm::vec3& b = v[idx[i + 1]];
            const glm::vec3& c = v[idx[i + 2]];
            const glm::vec3 n = glm::cross(b - a, c - a);
            const float len = glm::length(n);
            // A wall has no one height on the vertical.
            if (!(len > 1e-9f) || std::abs(n.z) < 1e-3f * len) continue;
            const auto side = [&](const glm::vec3& s, const glm::vec3& e) {
                return (e.x - s.x) * (p.y - s.y) - (e.y - s.y) * (p.x - s.x);
            };
            const float s0 = side(a, b), s1 = side(b, c), s2 = side(c, a);
            const bool inside = (s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) || (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f);
            if (!inside) continue;
            const float z = a.z - (n.x * (p.x - a.x) + n.y * (p.y - a.y)) / n.z;
            if (std::abs(z - p.z) > kMaxReach) continue;
            f(z, n.z / len);
        }
    }
}
}  // namespace detail

/// The height of the nearest face turned upward straight below `p` (the
/// model's z is up; a WMO's faces are wound so that the cross of their edges
/// is their outward normal). A ceiling's underside looks down and is passed
/// through, so a lamp under a ceiling finds its own room's floor, not the
/// floor of the room above.
inline std::optional<float> floorBelow(const glm::vec3& p, const std::vector<GroupMesh>& groups) {
    std::optional<float> best;
    const auto visit = [&](float z, float up) {
        if (up < kFloorMinUp || z > p.z + kFloorAbove) return;
        if (!best || z > *best) best = z;
    };
    detail::facesOnVertical(p, groups, visit);
    return best;
}

/// The height of the nearest face of any kind straight above `p`: its
/// ceiling, a beam, the underside of the floor above.
inline std::optional<float> ceilingAbove(const glm::vec3& p, const std::vector<GroupMesh>& groups) {
    std::optional<float> best;
    const auto visit = [&](float z, float) {
        if (z <= p.z + kFloorAbove) return;
        if (!best || z < *best) best = z;
    };
    detail::facesOnVertical(p, groups, visit);
    return best;
}

/// The storey of a light at `p`: none without a floor below it.
inline std::optional<Storey> storeyAt(const glm::vec3& p, const std::vector<GroupMesh>& groups) {
    const auto floorZ = floorBelow(p, groups);
    if (!floorZ) return std::nullopt;
    const auto ceilingZ = ceilingAbove(p, groups);
    return Storey{*floorZ, ceilingZ ? *ceilingZ : std::numeric_limits<float>::infinity()};
}

/// Whether a building light of storey `storey` (none: everywhere) shades the
/// baked colour of a surface at height `z` (rt_trace.comp.glsl's rule). A
/// surface that is no building's baked colour (`storeyBound` false) is
/// shaded as before, as is everything by a light without a storey.
inline bool shades(const std::optional<Storey>& storey, float z, bool storeyBound) {
    if (!storeyBound || !storey) return true;
    return z >= storey->floorZ - kMargin && z <= storey->ceilingZ + kMargin;
}

}  // namespace wowee::rendering::rt_light_storey
