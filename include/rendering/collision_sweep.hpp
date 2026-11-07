#pragma once

/// The client's sweep of a unit's body through the faces around it
/// (0x0075f9d0 and the pieces it calls), ported as it is.
///
/// The body is not a cylinder or a capsule. It is a box on a pyramid
/// (0x0075ca80, 0x0075c8f0): a square column of half-width r from
/// 1.849399 r above the feet to the height h, standing on four faces that
/// meet in a point at the feet. r is a third of a yard and h 73/36 for a
/// player of normal size. The pyramid is what lets the body ride up a slope
/// and over a lip: its faces lean 28.4 degrees from the vertical, so ground
/// less steep than 61.6 degrees meets the point first.
///
/// A sweep moves the body along a direction by up to a distance and stops
/// it at the first face in the way. For each of the body's faces that leads
/// the motion, the facets in front are cut down to the part the face passes
/// over (clipFacet, 0x0075b710), and the distance the face can go before it
/// touches what is left is solved for (solveFace, 0x0075c0b0). The least of
/// these is how far the body may move, and the body's faces that touched at
/// that distance - within 1/720 of a yard - are the contacts.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/collision_facets.hpp"

namespace wowee::rendering::collision {

/// 0xa37f14: how close counts as touching.
inline constexpr float kSkin = 1.0f / 720.0f;
/// 0xa3f854: the least a sweep looks ahead.
inline constexpr float kMinLookAhead = 1.0f / 36.0f;
/// 0x9f1224, 0x9ea27c.
inline constexpr float kTiny20 = 0x1p-20f;
inline constexpr float kTiny22 = 0x1p-22f;
/// 0xa32830: the column's foot above the point, in half-widths.
inline constexpr float kPyramidRise = 1.8493990f;
/// 0xa37f28, 0xa37f2c: the pyramid's face normals.
inline constexpr float kPyramidNormalXY = 0.87964189f;
inline constexpr float kPyramidNormalZ = 0.47563660f;
/// A player's body (0x006e9570 without a display's own size).
inline constexpr float kPlayerHalfWidth = 1.0f / 3.0f;
inline constexpr float kPlayerHeight = 73.0f / 36.0f;
/// 0x9e9ee4: the share of the height the liquid list's body keeps.
inline constexpr float kLiquidBodyShare = 0.75f;
/// 0xa405e0 and around: the gather box's margin round the swept body.
inline constexpr float kGatherPad = 1.0f / 6.0f;

struct Plane {
    glm::vec3 n{0.0f, 0.0f, 1.0f};
    float d = 0.0f;
    [[nodiscard]] float at(const glm::vec3& p) const { return glm::dot(n, p) + d; }
};

/// The body at a position (its feet): nine vertices and nine planes.
struct Body {
    float r = kPlayerHalfWidth;
    float h = kPlayerHeight;
    glm::vec3 feet{0.0f};
    glm::vec3 v[9]{};
    Plane p[9]{};

    Body() = default;
    Body(const glm::vec3& feetPos, float halfWidth, float height);
    void place(const glm::vec3& feetPos);
    [[nodiscard]] glm::vec3 boxMin() const;
    [[nodiscard]] glm::vec3 boxMax() const;
};

/// Which vertices each of the body's faces is made of (P0..P4 quads, P5..P8
/// triangles from the point); -1 pads a triangle.
inline constexpr int kBodyFaces[9][4] = {
    {1, 2, 6, 5}, {3, 4, 8, 7}, {2, 3, 7, 6}, {4, 1, 5, 8}, {5, 6, 7, 8},
    {0, 1, 2, -1}, {0, 3, 4, -1}, {0, 2, 3, -1}, {0, 4, 1, -1}};

/// 0x0075b710: the part of a polygon behind a plane (n.v + d < 0), with the
/// client's tolerances - kept whole if all of it is within 1/720 in front,
/// gone if none of it is more than 1/720 behind, and an edge's crossing
/// kept only where the vertex before it is more than 1/720 from the plane.
/// Fewer than three vertices left is nothing.
void clipFacet(std::vector<glm::vec3>& poly, const Plane& plane, std::vector<glm::vec3>& scratch);

/// 0x0075c0b0: how far along `dir` the body's face `face` goes before it
/// touches the polygon, which already lies in the face's path - nothing if
/// it already does. Writes `*best` and answers true when that is less than
/// it was. The polygon may be cut down.
bool solveFace(const Body& body, int face, const glm::vec3& dir, std::vector<glm::vec3>& poly,
               float* best, std::vector<glm::vec3>& scratch);

struct SweepResult {
    /// How far the body may go: the nearest touch, or the whole move, and
    /// nothing at all for a touch nearer than 1/720.
    float allowed = 0.0f;
    bool hit = false;
    /// The body's faces (0..8, Body::p) touching at that distance, the one
    /// that set it first.
    std::vector<int> contacts;
    /// A facet one of them touched: the one the last face to touch anything
    /// found, which is not always the nearest (0x0075c5a0 writes it for
    /// every face that finds one).
    int facet = -1;
};

/// 0x0075f9d0: move `body` along the unit `dir` by up to `dist` through the
/// facets, and answer how far it may go. The body is not moved.
SweepResult sweep(const Body& body, const glm::vec3& dir, float dist,
                  const std::vector<Facet>& facets);

/// The box 0x0075f0a0 gathers facets from for a sweep: the body at its
/// start and at its end, with 1/6 yard round both.
void sweepBox(const Body& body, const glm::vec3& dir, float dist, glm::vec3& outMin,
              glm::vec3& outMax);

}  // namespace wowee::rendering::collision
