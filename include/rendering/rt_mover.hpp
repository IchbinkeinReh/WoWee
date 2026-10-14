#pragma once

#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

/// Something drawn this frame that moves on its own - a unit, what it
/// carries, a door, a chest, a lift, a ship - as the ray traced lighting
/// takes it (RtLighting::FrameInputs::movers): the box it fills this frame
/// and how it moved since the last, as a rigid transform taking where a
/// point of it is now to where it was then. The G-buffer texels inside the
/// box are the mover's (rt_prepare); their history is found where the
/// transform puts them (rt_temporal), and the surfaces of the next frame do
/// not take them for what stood behind (rtLightAt).
struct RtMover {
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};
    glm::mat4 prevFromCur{1.0f};
    /// Whether it moved since the last frame, as a whole or in its pose: its
    /// texels keep a shorter history then (rt_temporal).
    bool moving = true;
    /// Drawn see-through (a unit fading, the player's body as the camera
    /// closes in): its texels are what stands behind it (rt_prepare).
    bool seeThrough = false;
    /// A unit, in the ray traced scene among the units (a group, which a
    /// ray can pass by): of the texels in its box, those the scene without
    /// the units also has at that depth are what stands beside it - a wall,
    /// a window frame, the floor - not it (rt_prepare).
    bool unit = false;
    /// What orders the list, the nearest first: the squared distance from
    /// the camera.
    float rank = 0.0f;
};

} // namespace rendering
} // namespace wowee
