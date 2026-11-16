#pragma once

/// Whether a unit's body still fits where it stands once it has grown: the
/// overlap queries the client runs when the active player's scale goes up
/// (0x0072a560), before it asks the server, with CMSG_CANCEL_GROWTH_AURA, to
/// take the growth away again.
///
/// Two shapes are tested, each against the faces the world gathers in a box
/// round it (ground, buildings and doodads: 0x00783a40 with the mover's
/// flags 0x100111, collision_facets.hpp):
///
///  - on foot (0x006d78c0 -> 0x006e8ff0 -> 0x0075e500): the movement body
///    itself, the box on a pyramid of collision_sweep.hpp, at the grown
///    size. A face is in the way when any of it is left inside all nine of
///    the body's planes (0x0075c800, clipped by 0x0075b710) other than the
///    point at the feet: what is left must reach more than 1/36 of a yard
///    from the feet (0x0075c6f0), so the ground the unit stands on does not
///    count.
///  - mounted (0x006d7720 -> 0x0075e480): only the slab the rider grows
///    into, a box from the top of the body as it is up to the grown top. A
///    face is in the way when any of it is inside the box (0x0075c750,
///    clipped to the box's six sides by 0x0075bb00).
///
/// The gather fails where the box leaves the map or reaches ground not
/// loaded, and the client reads a failed gather as no room
/// (0x0075e500 / 0x0075e480 answer 1).

#include <cmath>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "core/coordinates.hpp"
#include "rendering/collision_facets.hpp"
#include "rendering/collision_sweep.hpp"

namespace wowee::rendering::collision {

/// 0xa3282c: the most half-width the grown body is given, per unit of
/// scale - where the pyramid's rise (kPyramidRise) would take the column's
/// foot above one yard of scale.
inline constexpr float kFitHalfWidthCap = 0.54071623f;
/// 0x0075c6f0: (1/36)^2, how near the feet what is left of a face may lie
/// and still not count.
inline constexpr float kFitFeetTouchSq = kMinLookAhead * kMinLookAhead;
/// 0x007a5f20: the map's half extent (17066.666); a box past it gathers
/// nothing.
inline constexpr float kMapHalfExtent = core::coords::ZEROPOINT;

/// The grown body's size: CreatureModelData CollisionWidth and
/// CollisionHeight at the unit's collision scale (0x006e8ff0). The
/// half-width is the width's half at that scale, unless that times the
/// pyramid's rise is more than the scale itself, when it is the scale
/// times 0.54071623.
struct FitExtents {
    float halfWidth = 0.0f;
    float height = 0.0f;
};

inline FitExtents growthFitExtents(float modelWidth, float modelHeight, float scale) {
    FitExtents e;
    e.height = modelHeight * scale;
    const float half = modelWidth * scale * 0.5f;
    e.halfWidth = half * kPyramidRise <= scale ? half : scale * kFitHalfWidthCap;
    return e;
}

/// 0x0075c800 with 0x0075c6f0: whether a facet is in the body's way.
/// The triangle is cut down by each of the body's nine planes in turn; a
/// triangle with nothing left is clear, and so is one whose every remaining
/// corner lies within 1/36 of a yard of the feet.
inline bool facetInBody(const Body& body, const Facet& f, std::vector<glm::vec3>& poly,
                        std::vector<glm::vec3>& scratch) {
    poly.assign(std::begin(f.v), std::end(f.v));
    for (const Plane& plane : body.p) {
        clipFacet(poly, plane, scratch);
        if (poly.empty()) return false;
    }
    for (const glm::vec3& v : poly) {
        const glm::vec3 d = v - body.feet;
        if (glm::dot(d, d) > kFitFeetTouchSq) return true;
    }
    return false;
}

inline bool bodyOverlapsFacets(const Body& body, const std::vector<Facet>& facets) {
    std::vector<glm::vec3> poly;
    std::vector<glm::vec3> scratch;
    for (const Facet& f : facets) {
        if (facetInBody(body, f, poly, scratch)) return true;
    }
    return false;
}

/// 0x0075bb00: the six sides of a box as clip planes, in the client's
/// order: -x, +x, -y, +y, -z, +z, each keeping the inside.
inline void boxPlanes(const glm::vec3& lo, const glm::vec3& hi, Plane (&out)[6]) {
    out[0] = {{-1.0f, 0.0f, 0.0f}, lo.x};
    out[1] = {{1.0f, 0.0f, 0.0f}, -hi.x};
    out[2] = {{0.0f, -1.0f, 0.0f}, lo.y};
    out[3] = {{0.0f, 1.0f, 0.0f}, -hi.y};
    out[4] = {{0.0f, 0.0f, -1.0f}, lo.z};
    out[5] = {{0.0f, 0.0f, 1.0f}, -hi.z};
}

/// 0x0075c750: whether any facet has something left inside the box.
inline bool boxOverlapsFacets(const glm::vec3& lo, const glm::vec3& hi,
                              const std::vector<Facet>& facets) {
    Plane planes[6];
    boxPlanes(lo, hi, planes);
    std::vector<glm::vec3> poly;
    std::vector<glm::vec3> scratch;
    for (const Facet& f : facets) {
        poly.assign(std::begin(f.v), std::end(f.v));
        for (const Plane& plane : planes) {
            clipFacet(poly, plane, scratch);
            if (poly.empty()) break;
        }
        if (!poly.empty()) return true;
    }
    return false;
}

struct Box {
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};
};

/// The rider's height on its mount at collision scale 1 (0x006d7720): half
/// the rider model's CollisionHeight plus the mount model's MountHeight
/// times the mount display's scale (+0x990), or the rider model's own
/// height where that is more.
inline float mountedRiderHeight(float riderModelHeight, float mountHeight, float mountDisplayScale) {
    const float raised = riderModelHeight * 0.5f + mountDisplayScale * mountHeight;
    return riderModelHeight <= raised ? raised : riderModelHeight;
}

/// The box a mounted rider's growth is tested in (0x006d7720), or nothing
/// when it is not tested: where the rider model is taller than its raised
/// height, its own height stands and nothing is tested. Otherwise the box
/// is the body's current half-width round the feet, from the top of the
/// body as it is to the raised height at the grown collision scale.
inline std::optional<Box> mountedGrowthBox(const glm::vec3& feet, float halfWidth, float currentHeight,
                                           float riderModelHeight, float mountHeight,
                                           float mountDisplayScale, float newScale) {
    const float raised = riderModelHeight * 0.5f + mountDisplayScale * mountHeight;
    if (!(riderModelHeight <= raised)) return std::nullopt;
    Box b;
    b.lo = feet + glm::vec3(-halfWidth, -halfWidth, currentHeight);
    b.hi = feet + glm::vec3(halfWidth, halfWidth, raised * newScale);
    return b;
}

/// 0x007a5f20: the gather answers nothing for a box not wholly inside the
/// map.
inline bool boxInsideMap(const glm::vec3& lo, const glm::vec3& hi) {
    return lo.x >= -kMapHalfExtent && lo.y >= -kMapHalfExtent && hi.x <= kMapHalfExtent &&
           hi.y <= kMapHalfExtent;
}

/// 0x0072a560: the client tests the fit only while the unit grows - the new
/// scale neither below nor equal to the old (a NaN either side counts as
/// growing, as the x87 compare falls through).
inline bool scaleGrew(float newScale, float oldScale) {
    return !(newScale < oldScale || newScale == oldScale);
}

}  // namespace wowee::rendering::collision
