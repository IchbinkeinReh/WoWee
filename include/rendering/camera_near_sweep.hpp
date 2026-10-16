#pragma once

/// Where the camera's collision lets it be, as the client sweeps it
/// (Camera.cpp 0x00605d60).
///
/// The line from the pivot to the camera is cut where it first meets the world
/// (0x0077f310). Then the near clip's rectangle - at its own size and at 1.75
/// times it, about its centre (0x005ff670) - is swept from the pivot along the
/// same line, and the camera comes in to where the rectangle first touches
/// anything (0x006059e0 -> 0x006057b0). Whatever shortened the line, the camera
/// is drawn in a further ninth of a yard. So the near plane, which lies between
/// the camera and the pivot inside the swept volume, never reaches into a wall:
/// a single ray kept the camera's centre off the wall it looked along, but
/// a wall it slid past at an angle cut through the near plane's corners and the
/// building's inside showed.

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/glm.hpp>

namespace wowee::rendering::camera_sweep {

/// The ninth of a yard the client takes off a distance the sweep shortened.
inline constexpr float kPullback = 1.0f / 9.0f;
/// The larger of the two rectangles the client sweeps, over the near clip's.
inline constexpr float kNearRectScale = 1.75f;

/// The near clip's half-extents (right, up) for a vertical field of view in
/// degrees and a width-over-height aspect.
inline glm::vec2 nearHalfExtents(float nearClip, float fovYDegrees, float aspect) {
    const float halfH = nearClip * std::tan(glm::radians(fovYDegrees) * 0.5f);
    return {halfH * aspect, halfH};
}

/// The points of the swept rectangle the sweep is sampled at, as (right, up)
/// offsets from the line: its corners and the middles of its sides, at the
/// larger of the client's two sizes - which holds the smaller.
inline std::array<glm::vec2, 8> sweepOffsets(float nearClip, float fovYDegrees, float aspect) {
    const glm::vec2 h = nearHalfExtents(nearClip, fovYDegrees, aspect) * kNearRectScale;
    return {{{-h.x, -h.y}, {h.x, -h.y}, {h.x, h.y}, {-h.x, h.y},
             {0.0f, -h.y}, {h.x, 0.0f}, {0.0f, h.y}, {-h.x, 0.0f}}};
}

/// The camera's distance once the sweep is done: the nearest of what the line
/// and the rectangle's sample points met (each at most `wanted`), less the
/// ninth of a yard when that is short of `wanted`, and never under `minimum`.
inline float sweptDistance(float wanted, float nearestHit, float minimum) {
    if (nearestHit >= wanted) return wanted;
    return std::max(minimum, nearestHit - kPullback);
}

}  // namespace wowee::rendering::camera_sweep
