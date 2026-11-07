#pragma once

/// The faces a moving unit is swept against, gathered the way the client
/// gathers them for its movement (0x0075ff90 -> 0x00783910 -> 0x007a5f20).
///
/// This is the first piece of a port of the client's collision. Everything a
/// sweep meets is one record of 0x34 bytes: a plane and the triangle on it.
/// The plane is n.x + d = 0 with d = -n.v0, and a facet counts only from the
/// side its normal points to - a sweep takes it only if the motion runs into
/// it, n.dir <= -1e-5 (0xa34eec). So which way a normal points is the whole
/// of whether a face is solid, and each source below says where its normal
/// comes from:
///
///  - terrain (0x007d8840): four triangles fanned from each quad's centre,
///    holes left out, normal from the winding and normalized (0x007912c0);
///    a heightfield's always points up.
///  - buildings (0x007a55e0, group 0x007aef00, triangles 0x007cb7b0 and
///    0x00782740): the winding in MOVI order, turned by the instance's
///    matrix and normalized, (0,0,1) for a degenerate one. A group flagged
///    0x80 is left out, and so is a detail face (MOPY 0x04); the client's
///    mask is 0x84, whose 0x80 is the mark it puts on a face already taken.
///    A face's 0x08 is not looked at: rendered faces collide.
///  - doodads (0x007a50c0 -> 0x007a4af0 -> 0x0082ec30): the file's bounding
///    triangles, their vertices through the whole matrix and their normals
///    from the file, turned by the matrix's axes each scaled to length one,
///    and not normalized after. A model not yet loaded stands in as the
///    twelve triangles of its box (0x007a3e40).
///
/// A triangle the query box does not reach is dropped only when all three
/// corners lie beyond the same side of it - the client's outcode test - so
/// a long face across the box is kept.

#include <cstdint>
#include <cstdlib>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::collision {

/// WOWEE_COLLISION_PORT=1 turns the port on. Off by default until it has
/// been walked in the world.
inline bool portEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("WOWEE_COLLISION_PORT");
        return v && v[0] && v[0] != '0';
    }();
    return on;
}

/// One record of 0xadba34's list.
struct Facet {
    glm::vec3 n{0.0f, 0.0f, 1.0f};
    float d = 0.0f;
    glm::vec3 v[3]{};
};

/// 0xa34eec: a facet is met only by a motion into its front.
inline constexpr float kFacingEpsilon = 1e-5f;

inline bool facesMotion(const Facet& f, const glm::vec3& dir) {
    return glm::dot(f.n, dir) <= -kFacingEpsilon;
}

inline Facet makeFacet(const glm::vec3& n, const glm::vec3& a, const glm::vec3& b,
                       const glm::vec3& c) {
    Facet f;
    f.n = n;
    f.d = -glm::dot(n, a);
    f.v[0] = a;
    f.v[1] = b;
    f.v[2] = c;
    return f;
}

/// The normal of a triangle by its winding, (b-a) x (c-a) made unit length;
/// (0,0,1) where the triangle has no area (0x00782740).
inline glm::vec3 windingNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    if (!(len > 0.0f)) return {0.0f, 0.0f, 1.0f};
    return n / len;
}

/// Which sides of a box a point lies beyond, one bit a side.
inline uint32_t outcode(const glm::vec3& p, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    uint32_t code = 0;
    if (p.x < boxMin.x) code |= 0x01u;
    if (p.x > boxMax.x) code |= 0x02u;
    if (p.y < boxMin.y) code |= 0x04u;
    if (p.y > boxMax.y) code |= 0x08u;
    if (p.z < boxMin.z) code |= 0x10u;
    if (p.z > boxMax.z) code |= 0x20u;
    return code;
}

/// A triangle is left out only when all three corners are beyond one side.
inline bool outsideBox(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                       const glm::vec3& boxMin, const glm::vec3& boxMax) {
    return (outcode(a, boxMin, boxMax) & outcode(b, boxMin, boxMax) &
            outcode(c, boxMin, boxMax)) != 0;
}

/// 0x007aef00 passes over a group flagged 0x80.
inline bool groupCollides(uint32_t mogpFlags) { return (mogpFlags & 0x80u) == 0; }

/// The mask 0x84 a player's query makes, less its visited mark 0x80: a
/// detail face is not there for it.
inline bool buildingFaceCollides(uint8_t mopy) { return (mopy & 0x04u) == 0; }

/// The facets of one terrain quad: four triangles from its centre `c` to
/// each edge of its corners `p`, which go round it either way. The client
/// winds them (0xa405e8) so their normals point up, and a heightfield's
/// faces can only face up, so a winding that comes out facing down is
/// turned over. Those the box does not reach are left out.
inline void appendTerrainQuad(const glm::vec3& c, const glm::vec3 (&p)[4], const glm::vec3& boxMin,
                              const glm::vec3& boxMax, std::vector<Facet>& out) {
    for (int e = 0; e < 4; ++e) {
        const glm::vec3& a = p[e];
        const glm::vec3& b = p[(e + 1) & 3];
        if (outsideBox(c, a, b, boxMin, boxMax)) continue;
        const glm::vec3 n = windingNormal(c, a, b);
        if (n.z < 0.0f) {
            out.push_back(makeFacet(-n, c, b, a));
        } else {
            out.push_back(makeFacet(n, c, a, b));
        }
    }
}

/// A building's triangle, its corners already in the world: the winding's
/// normal, which for a matrix that only turns and moves is the local one
/// turned.
inline void appendBuildingTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                   const glm::vec3& boxMin, const glm::vec3& boxMax,
                                   std::vector<Facet>& out) {
    if (outsideBox(a, b, c, boxMin, boxMax)) return;
    out.push_back(makeFacet(windingNormal(a, b, c), a, b, c));
}

/// The matrix's three axes, each made unit length: what a doodad's file
/// normals are turned by. A uniform scale drops out; nothing renormalizes
/// the result.
inline glm::mat3 unitAxes(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(r[i]);
        if (len > 0.0f) r[i] /= len;
    }
    return r;
}

/// A doodad's bounding triangle, in its own space, with the file's normal
/// for it. `fileNormal` null means the file has none for this one, and the
/// winding stands in.
inline void appendDoodadTriangle(const glm::mat4& model, const glm::mat3& axes,
                                 const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                 const glm::vec3* fileNormal, const glm::vec3& boxMin,
                                 const glm::vec3& boxMax, std::vector<Facet>& out) {
    const glm::vec3 wa(model * glm::vec4(a, 1.0f));
    const glm::vec3 wb(model * glm::vec4(b, 1.0f));
    const glm::vec3 wc(model * glm::vec4(c, 1.0f));
    if (outsideBox(wa, wb, wc, boxMin, boxMax)) return;
    const glm::vec3 n = fileNormal ? axes * *fileNormal : windingNormal(wa, wb, wc);
    out.push_back(makeFacet(n, wa, wb, wc));
}

/// The twelve facets of a box, facing out (0x007a3e40): what a doodad not
/// yet loaded is.
inline void appendBoxFacets(const glm::vec3& lo, const glm::vec3& hi, std::vector<Facet>& out) {
    const glm::vec3 k[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z},
                            {lo.x, hi.y, lo.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
                            {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    // Each side as a quad wound anticlockwise seen from outside.
    static constexpr int kSides[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                         {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    for (const auto& s : kSides) {
        const glm::vec3 n = windingNormal(k[s[0]], k[s[1]], k[s[2]]);
        out.push_back(makeFacet(n, k[s[0]], k[s[1]], k[s[2]]));
        out.push_back(makeFacet(n, k[s[0]], k[s[2]], k[s[3]]));
    }
}

}  // namespace wowee::rendering::collision
