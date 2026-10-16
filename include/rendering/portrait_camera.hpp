#pragma once

/// How a unit's portrait is framed and when it is drawn (PortraitButton.cpp).
///
/// SetPortraitTexture (0x00619580) draws the unit's model once into a 64x64
/// texture, kept and reused until the unit's look changes - a still picture,
/// not a live view. It looks through the model's own portrait camera
/// (camera 0, 0x008278e0 / 0x00827960), as the artist placed it: for the
/// player races that is the head and nothing else.
///
/// A camera's field of view is the diagonal one: the projection divides it
/// by the square root of one plus the aspect squared before taking the
/// vertical one (0x006bfe00 -> 0x006bf370).

#include <cmath>

namespace wowee::rendering::portrait_camera {

/// The vertical field of view, radians, of a camera whose field of view is
/// `diagonal` radians, drawing into a target `aspect` (width over height).
inline float verticalFov(float diagonal, float aspect) {
    return diagonal / std::sqrt(1.0f + aspect * aspect);
}

/// How far back from the artist's camera an animated portrait stands, along
/// its own line, so the idle does not swing the head out of the picture. A
/// still one is taken from where the artist put it.
constexpr float kAnimatedHeadroom = 1.35f;
constexpr float kStillHeadroom = 1.0f;

/// A still portrait is drawn on this many frames after its model is (re)built
/// and then left: the first draws it, the rest cover the frames in flight
/// that still held the old picture.
constexpr int kStillFrames = 3;

}  // namespace wowee::rendering::portrait_camera
