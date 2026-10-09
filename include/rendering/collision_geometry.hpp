#pragma once

/// The two primitives every collision query in this client is built from, and
/// the focus that decides which instances a query looks at.
///
/// The doodad renderer and the building renderer each had their own copy of
/// all three. They agreed, which is the point: a difference here does not
/// crash or log, it moves where the world is solid. A ray test that starts
/// rejecting backfaces makes floors one-sided; a distance test that is off by
/// its own radius drops the instance a character is standing on out of the
/// query, and the character falls through it.
///
/// The focus is the one with the most copies: two setters and four tests, the
/// tests written inline three times in the doodad renderer and once as a
/// method in the building one.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// Squared distance from `p` to the nearest point of the box.
///
/// Zero inside the box, which is what the callers rely on: an instance the
/// query point is inside is never further away than one it is outside.
inline float pointAABBDistanceSq(const glm::vec3& p, const glm::vec3& bmin,
                                 const glm::vec3& bmax) {
    const glm::vec3 nearest = glm::clamp(p, bmin, bmax);
    const glm::vec3 d = p - nearest;
    return glm::dot(d, d);
}

/// Moller-Trumbore. Distance along `dir` to the triangle, or negative for a
/// miss.
///
/// Two-sided on purpose. A floor is a triangle whose winding says which way
/// is up, and a character standing on the underside of a bridge or inside a
/// building still needs it to be solid, so a version that culled backfaces
/// would make half the world's surfaces one-way.
inline float rayTriangleIntersect(const glm::vec3& origin, const glm::vec3& dir,
                                  const glm::vec3& v0, const glm::vec3& v1,
                                  const glm::vec3& v2) {
    constexpr float EPSILON = 1e-6f;
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 h = glm::cross(dir, e2);
    const float a = glm::dot(e1, h);
    if (a > -EPSILON && a < EPSILON) return -1.0f;  // ray parallel to the plane

    const float f = 1.0f / a;
    const glm::vec3 s = origin - v0;
    const float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return -1.0f;

    const glm::vec3 q = glm::cross(s, e1);
    const float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return -1.0f;

    const float t = f * glm::dot(e2, q);
    return t > EPSILON ? t : -1.0f;  // behind the origin counts as a miss
}

// Closest point on triangle to a point (Ericson, Real-Time Collision Detection §5.1.5).
inline glm::vec3 closestPointOnTriangle(const glm::vec3& p,
                                         const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }
    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

/// Hold an upright cylinder - a character - clear of one wall triangle at the
/// end of a step from `from` to `to`, moving `to` sideways only. Answers
/// whether it moved. Everything is in the triangle's own frame; `to.z` and
/// `from.z` are the feet.
///
/// The client sweeps the character's box along the step against the
/// collision faces it gathered (0x0075ff90 gathers, 0x0075f9d0 sweeps) and
/// stops it where it first touches, 1/720 short; what is left of the step
/// slides along the face. So a face holds however thin the thing behind it
/// and however long the step. This is that, for a cylinder:
///
/// - A face holds back only what is in front of it. The client tests a face
///   only when the step runs into its front (0x0075c5a0 skips one whose
///   normal does not oppose the motion), so a plate - a door, a gate - is
///   held by the face toward the character and the one behind it plays no
///   part. Pushed out of whichever face is nearest instead, a character past
///   the middle of a plate comes out on its far side: walking through it.
/// - A step whose centre crosses the face's plane where the face is, is
///   brought back to the front, a radius off. Only the part of the step into
///   the face is taken away, so the rest of it slides along.
/// - Otherwise all of the overlap goes, not a fraction of it. A push smaller
///   than the step is a wall the character walks through a little slower.
inline bool holdCylinderOffWallTriangle(const glm::vec3& from, glm::vec3& to,
                                        float radius, float height,
                                        const glm::vec3& v0, const glm::vec3& v1,
                                        const glm::vec3& v2) {
    constexpr float kSkin = 1.0f / 720.0f;   // the client's own contact gap

    // The part of the body level with the triangle.
    const float triMinZ = std::min({v0.z, v1.z, v2.z});
    const float triMaxZ = std::max({v0.z, v1.z, v2.z});
    const float lo = std::max(to.z, triMinZ);
    const float hi = std::min(to.z + height, triMaxZ);
    if (lo > hi) return false;

    glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
    const float nLen = glm::length(n);
    if (nLen < 1e-8f) return false;
    n /= nLen;
    const float nxyLen = std::sqrt(n.x * n.x + n.y * n.y);
    if (nxyLen < 1e-3f) return false;   // flat: the floor's business

    // Horizontal distance from the face's plane at height z, signed.
    auto planeDist = [&](const glm::vec3& p, float z) {
        return glm::dot(glm::vec3(p.x, p.y, z) - v0, n) / nxyLen;
    };
    // Behind the face: it does not hold.
    if (planeDist(from, 0.5f * (lo + hi)) < 0.0f) return false;
    const glm::vec2 nh = glm::vec2(n.x, n.y) / nxyLen;

    // A leaning face is nearest the body at one end of the overlap.
    const float zc = planeDist(to, lo) <= planeDist(to, hi) ? lo : hi;
    const float toDist = planeDist(to, zc);
    const float fromDist = planeDist(from, zc);

    if (toDist < 0.0f && fromDist >= 0.0f) {
        // The centre went through the plane. Where it did, was the face there?
        const float t = fromDist / (fromDist - toDist);
        const glm::vec3 a(from.x, from.y, zc);
        const glm::vec3 b(to.x, to.y, zc);
        const glm::vec3 q = a + (b - a) * t;
        const glm::vec3 c = closestPointOnTriangle(q, v0, v1, v2);
        if (glm::dot(q - c, q - c) < radius * radius) {
            const float back = radius + kSkin - toDist;
            to.x += nh.x * back;
            to.y += nh.y * back;
            return true;
        }
        // It passed beside the face; its edge is handled below.
    }

    const glm::vec3 probe(to.x, to.y, zc);
    const glm::vec3 c = closestPointOnTriangle(probe, v0, v1, v2);
    const glm::vec2 d(probe.x - c.x, probe.y - c.y);
    const float distXY = glm::length(d);
    if (distXY >= radius) return false;
    const glm::vec2 dir = distXY > 1e-4f ? d / distXY : nh;
    const float push = radius + kSkin - distXY;
    to.x += dir.x * push;
    to.y += dir.y * push;
    return true;
}

/// The sphere a collision query is restricted to, so that a query near the
/// player does not walk every instance in the world.
///
/// Radius zero means no restriction rather than an empty one: that is what
/// both renderers meant by an unset focus, and reading it the other way would
/// make the world non-solid rather than merely slow.
struct CollisionFocus {
    bool enabled = false;
    glm::vec3 position{0.0f};
    float radius = 0.0f;
    float radiusSq = 0.0f;

    void set(const glm::vec3& worldPos, float newRadius) {
        enabled = (newRadius > 0.0f);
        position = worldPos;
        radius = std::max(0.0f, newRadius);
        radiusSq = radius * radius;
    }

    /// Whether an instance with these world bounds is outside the focus and
    /// can be skipped. Always false while unset.
    ///
    /// The box is tested rather than its centre, so a long building whose
    /// origin is far away is still collided while the player stands on one
    /// end of it.
    [[nodiscard]] bool excludes(const glm::vec3& boundsMin, const glm::vec3& boundsMax) const {
        return enabled &&
               pointAABBDistanceSq(position, boundsMin, boundsMax) > radiusSq;
    }
};

}  // namespace wowee::rendering
