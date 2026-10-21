#pragma once

// A unit's upper body on an animation of its own while the rest goes on
// with another: a cast or a swing on the move, the legs still running.
//
// The client keeps three animations for a unit: its mount model's, its own
// model's body, and the bone set at its upper-body key bone (+0xb84,
// SpineLow else the head, picked as the model loads - 0x0073e840). Every
// animation it is asked goes through 0x007385c0, which decides from what
// each plays now and how the unit moves which of them takes it, and which
// hands what it plays on to another: a swing asked of a running unit goes to
// the upper body; the run asked of a unit mid-swing takes the body and the
// swing goes on above it; the stand asked as it stops gives the swing back
// to the whole body. Whether the upper body may differ at all is 0x00723e30.
// What an upper-body one-shot ends into is 0x00737bd0's: the bone set goes
// back to following the body, blended over 150 ms (0x00832840).

#include "rendering/animation/animation_data.hpp"

#include <cstdint>

namespace wowee {
namespace rendering {
namespace upper_body {

/// The movement flags (+0x7cc, MOVEMENTFLAG_*) 0x00723e30 and 0x007385c0
/// read.
inline constexpr uint32_t kMoveDirection = 0x0000000f;  ///< forward, backward, strafing
inline constexpr uint32_t kMoveStrafe = 0x0000000c;
inline constexpr uint32_t kMoveTurn = 0x00000030;
inline constexpr uint32_t kMoveFalling = 0x00001000;
inline constexpr uint32_t kMoveSwimming = 0x00200000;
inline constexpr uint32_t kMoveFlying = 0x02000000;
/// Direction, turning, pitch; swimming, ascending, descending, flying.
inline constexpr uint32_t kMoveAny = 0x02e000ff;
/// The same with no turning or pitch.
inline constexpr uint32_t kMoveTravel = 0x02e0000f;
inline constexpr uint32_t kMoveSwimOrFly = kMoveSwimming | kMoveFlying;

/// What 0x007385c0 and 0x00723e30 read of the unit itself.
struct Unit {
    uint32_t moveFlags = 0;
    /// 0x00723350: falling with a fall speed, or on a falling spline.
    bool leaping = false;
    /// +0xa38 0x1800: turning on the spot (0x0073dab0).
    bool turningInPlace = false;
    /// vtable +0x138: UNIT_FIELD_BYTES_1 byte 0.
    uint8_t standState = 0;
    /// UNIT_FIELD_MOUNTDISPLAYID (+0xfc) set.
    bool mounted = false;
    /// +0xa38 0x400 with no UNIT_CHANNEL_SPELL: casting a spell with a cast
    /// time (0x007fa2e0 sets it where SpellCastTimes has one).
    bool castTimed = false;
    /// UNIT_CHANNEL_SPELL (+0x40).
    bool channeling = false;
    /// +0xa20: a melee target (SMSG_ATTACKSTART, 0x00754ff0).
    bool autoAttacking = false;
    /// +0xb5c == 2: the ranged weapon out.
    bool rangedDrawn = false;
    /// +0xa38 0x20000: CreatureModelData Flags 0x8, a model that moves only
    /// as a whole (0x0072d940, 0x0073e410).
    bool wholeBodyOnly = false;
    /// +0x98c: on a mount model, whose animations it plays while it holds
    /// its pose.
    bool riding = false;
    /// +0xb7c: the pose it holds there (Mount, or a mount aura kit's), and
    /// what 0x007176f0 makes of it for the model.
    uint32_t riderPose = 91;
    uint32_t riderPoseShown = 91;
    /// What 0x007176f0 makes of SwimIdle for the model.
    uint32_t swimIdle = 41;
    /// In its seat (state 3, 0x004f6250) with the seat's ride animation on
    /// its body (0x00747b20 not 0x1fa).
    bool seatedRide = false;
    /// Health 1 or more (+0x48), which the seat's hold on the body asks.
    bool alive = true;
};

/// What a slot plays, from where (0x008266b0's state): -1 for an upper body
/// with nothing of its own.
struct Slot {
    int32_t anim = -1;
    float timeMs = 0.0f;
    bool loop = true;
};

struct Slots {
    Slot body;
    Slot upper;
};

/// What 0x0073b510 asks when the body's one-shot has ended (+0xa38 0x40
/// alone): the upper body is left as it is.
enum class Pass : uint8_t { Asked, BodyEnded };

enum class Action : uint8_t {
    Keep,     ///< what plays goes on
    Play,     ///< the slot starts what the decision holds
    Release,  ///< the upper body follows the body again (0x00832840)
};

struct Decision {
    Slot body;
    Slot upper;
    Action bodyAction = Action::Keep;
    Action upperAction = Action::Keep;
    /// 0x00737ef0's blend: from the pose before, else straight in.
    bool bodyBlend = true;
    bool upperBlend = true;
    /// Asked of a rider, it is the mount model's to play (0x0071d6b0).
    bool toMount = false;
    /// What one slot played the other takes up, from where it had got.
    bool bodyFromUpper = false;
    bool upperFromBody = false;
};

// The animation lists 0x007385c0 and 0x00723e30 test, by BehaviorID
// (0x007176b0) except the first, which takes the id as asked.

/// 0x00714e80: moving - walking, running, strafing, jumping, swimming,
/// flying, sprinting, the stealth walk and run.
[[nodiscard]] bool isMovement(uint32_t anim);
/// 0x0071d940: the emotes.
[[nodiscard]] bool isEmote(uint32_t behavior);
/// 0x0071d590: swings, parries, blocks, dodges and the specials.
[[nodiscard]] bool isAttack(uint32_t behavior);
/// 0x0071d380: the spell casts.
[[nodiscard]] bool isSpell(uint32_t behavior);
/// 0x0071dae0: the combat readies (25-29).
[[nodiscard]] bool isReady(uint32_t behavior);
/// 0x0071d410: the spell readies (51, 52).
[[nodiscard]] bool isReadySpell(uint32_t behavior);
/// 0x0071d7c0: the jump (37-39, JumpLandRun).
[[nodiscard]] bool isJump(uint32_t behavior);
/// 0x0071d2a0: the jump's landing (JumpEnd, JumpLandRun).
[[nodiscard]] bool isLanding(uint32_t behavior);
/// 0x0071de50: fishing (133, 134).
[[nodiscard]] bool isFishing(uint32_t behavior);
/// 0x0071dde0: dying and dead.
[[nodiscard]] bool isDeath(uint32_t behavior);
/// 0x0071d550: the whole-body specials (Special1H, Special2H,
/// SpecialUnarmed).
[[nodiscard]] bool isWholeBodySpecial(uint32_t behavior);
/// 0x0071d800: what an upper body plays of its own.
[[nodiscard]] bool isUpperBody(uint32_t behavior);
/// 0x0071d6b0: what a rider's mount plays for it.
[[nodiscard]] bool isMountAnimation(uint32_t behavior);

/// 0x00723e30: whether `asked` may go to the upper body while `current` -
/// the upper body's, else the body's - plays.
[[nodiscard]] bool splits(const anim::AnimationDataTable& data, const Unit& unit, uint32_t asked, int32_t current);

/// 0x007385c0: where `asked` (0x007176f0's already) goes, and what each
/// slot plays after it.
[[nodiscard]] Decision decide(const anim::AnimationDataTable& data, const Unit& unit, const Slot& asked,
                              const Slots& now, Pass pass = Pass::Asked);

}  // namespace upper_body
}  // namespace rendering
}  // namespace wowee
