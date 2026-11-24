#pragma once

// What the mount under a rider plays.
//
// A unit that rides is asked its animations as any other: its base animation
// (0x0073ac30 -> 0x00724500) on each event of its movement (0x0073ed10), a
// jump's JumpStart, a fall far's Fall (0x0073ad00), a landing's JumpEnd or
// JumpLandRun (0x0073d2b0). The model that shows it is its mount's (vtable
// +0xd4, 0x006e6f80: +0x98c, else its own), and 0x007385c0 hands what is
// asked to that where it is one of a mount's (0x0071d6b0) while the rider's
// own body keeps its pose (+0xb7c). A mount has no list of its own: it walks,
// runs, sprints, backs up, jumps, falls, swims and flies as its rider's
// movement flags have it - so one in water swims (Swim, SwimLeft, SwimRight,
// SwimBackwards moving, SwimIdle still: 0x00717050, 0x0071e340), and one that
// falls into it swims from MSG_MOVE_START_SWIM on, which ends the fall
// (0x00989660 -> 0x00988490) and asks the base animation (0x0073ed10 case
// 0xca). Where its model has no such sequence it plays the one AnimationData's
// Fallbacks lead to (0x00826350): Walk for Swim, Stand for SwimIdle.
//
// A one-shot of the mount's ends as its rider's would (0x0073c140 ->
// 0x0073bff0 -> 0x0073b510), with the mount alone free to be given what
// follows (+0xa38 0x10).

#include "rendering/animation/base_animation.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace wowee {
namespace rendering {
namespace rider_mount {

/// 0x0071d6b0: whether the mount takes an animation asked of its rider.
[[nodiscard]] bool takes(const anim::AnimationDataTable& data, uint32_t animation);

/// The base animation asked of the mount alone (+0xa38 0x10), for `rider`
/// whose own body plays its pose: what the mount is left in where what the
/// whole unit is asked is none of a mount's - a combat ready or an emote
/// state, which are the rider's upper body's - once the pass it plays is over
/// (0x0073bff0). `flags` are those 0x007385c0 is asked with. `has` is the
/// model's 0x00825e00, `hasShown` the same for what 0x007176f0 makes of an id.
[[nodiscard]] base_animation::Pick base(const anim::AnimationDataTable& data, upper_body::Unit rider, uint32_t flags,
                                        const std::function<bool(uint32_t)>& has,
                                        const std::function<bool(uint32_t)>& hasShown);

/// 0x0073bff0 -> 0x0073b510: what follows the mount's one-shot `ended`.
[[nodiscard]] base_animation::Pick afterOneShot(const anim::AnimationDataTable& data, const upper_body::Unit& rider,
                                                uint32_t ended, const std::function<bool(uint32_t)>& has,
                                                const std::function<bool(uint32_t)>& hasShown);

}  // namespace rider_mount
}  // namespace rendering
}  // namespace wowee
