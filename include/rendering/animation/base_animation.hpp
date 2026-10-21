#pragma once

// The animation a unit is in when nothing else is asked of it: 0x00724500.
//
// The client asks it whenever what a unit does changes - it stops, starts,
// lands, its one-shot ends (0x0073b510 -> 0x0073ac30), its upper body's
// one-shot ends (0x00737bd0) - and plays what it answers as any animation
// asked (0x007385c0). It goes down a list and the first that has a say
// answers: dead, under water, falling, moving, casting, fighting, turning on
// the spot, the stand state, the emote state, and at the end the stand (or
// the swim idle, the stealth stand, the hover). One that has a say but
// leaves what plays answers kKeep.

#include "rendering/animation/upper_body.hpp"

#include <cstdint>
#include <functional>

namespace wowee {
namespace rendering {
namespace base_animation {

/// 0x1fa: what plays goes on.
inline constexpr uint32_t kKeep = anim::kNoBehavior;

/// +0xa38 0x10, 0x20, 0x40: the mount, the upper body and the body may be
/// given a new animation. 0x0073bbd0 leaves only the body's set while it
/// asks for what follows a body's one-shot.
inline constexpr uint32_t kMountFree = 0x10;
inline constexpr uint32_t kUpperFree = 0x20;
inline constexpr uint32_t kBodyFree = 0x40;
inline constexpr uint32_t kAllFree = 0x70;

/// What plays: 0x00717260's - the upper body's own, else the body's - and
/// 0x007172b0's, the body's. -1 for nothing.
struct Playing {
    int32_t shown = -1;
    int32_t body = -1;
};

struct Ask {
    uint32_t free = kAllFree;
    /// The flags 0x007385c0 will be asked with: 0x10 from a body's end
    /// (0x0073b510 -> 0x0073ac30(0x10)), 0 from an upper body's.
    uint32_t flags = 0;
    /// +0xb88: a swing asked over a swing, waiting (0x007385c0).
    int32_t queuedAttack = -1;
};

struct Pick {
    uint32_t anim = kKeep;
    /// Played once: the swing that waited, the death's start, a sit-down
    /// still going.
    bool loop = true;
    /// What the cast loop's kit Flags add to the flags 0x007385c0 is asked
    /// with (0x00714ce0): 0x4 -> 0x2, 0x8 -> 0x4 (neither puts the unit in
    /// it by its tier), 0x10 -> 0x8, 0x100 -> 0x20.
    uint32_t flags = 0;
};

/// What 0x00714dd0 reads of an item in a hand (vtable +300): its class and
/// subclass.
struct HandItem {
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
};

/// 0x00714dd0: the combat ready for the weapons in hand - the main hand's,
/// else the off hand's where that is the weapon (class 2): Ready1H, Ready2H
/// (Ready1H for a two-hander with something in the off hand), Ready2HL for a
/// polearm, staff or spear, else ReadyUnarmed.
[[nodiscard]] uint32_t meleeReady(const HandItem* mainHand, const HandItem* offHand);

/// 0x00724500 over everything it is asked from (mask -1). `has` answers
/// whether the model has a sequence of an id (0x00825e00); `hasShown`
/// whether it has the one 0x007176f0 puts the unit in for an id (0x0071dfc0).
/// The unit's speed is `unit.speed`.
[[nodiscard]] Pick pick(const anim::AnimationDataTable& data, const upper_body::Unit& unit, const Playing& playing,
                        const Ask& ask, const std::function<bool(uint32_t)>& has,
                        const std::function<bool(uint32_t)>& hasShown);

}  // namespace base_animation
}  // namespace rendering
}  // namespace wowee
