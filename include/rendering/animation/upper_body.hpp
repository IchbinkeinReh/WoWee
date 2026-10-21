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
#include <optional>

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
    /// +0xa38 0x400: casting a spell whose PrecastKit has a StartAnimID or
    /// AnimID above 0 (0x007fa2e0 -> 0x00715af0; cleared as the cast ends,
    /// 0x0072ed80). Without the kits, any cast with a cast time.
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
    /// A vehicle whose upper-body key bone plays an animation of its seats'
    /// (+0xf5c +0x58 has its bit, 0x00756cd0): that bone is the vehicle's,
    /// and nothing asked of the unit changes it.
    bool vehicleHoldsUpper = false;

    // What the base animation (0x00724500, base_animation.hpp) reads
    // besides.
    /// UNIT_FIELD_FLAGS_2 0x1 (+0xd8): feigning death, dead to 0x0071f560.
    bool feignDeath = false;
    /// GetCurrentSpeed (0x00987570), and the walk speed (+0x818): Walk at
    /// or under twice it, Run over, Sprint over 11 (0x00717050).
    float speed = 0.0f;
    float walkSpeed = 2.5f;
    /// UNIT_FIELD_BYTES_1 byte 2 0x2 (+0x112): stealthed - the stealth walk
    /// and stand.
    bool stealthed = false;
    /// 0x0071c6c0: hovering (MOVEMENTFLAG_HOVER 0x40000000) or with gravity
    /// off (0x400).
    bool hovering = false;
    /// Turning left on the spot (+0xa38 0x800).
    bool turningLeft = false;
    /// 0x00714dd0: the combat ready for its weapons (meleeReady).
    uint32_t meleeReady = 25;
    /// 0x007171c0: the AnimID of its UNIT_NPC_EMOTESTATE's Emotes row.
    std::optional<uint32_t> emoteStateAnim;
    /// 0x007224d0: the loop its cast or channel holds it in (the kit's
    /// AnimID) and the kit's Flags (0x00724330).
    std::optional<uint32_t> castLoopAnim;
    uint32_t castLoopKitFlags = 0;
    /// 0x0071e400: the AnimID of the newest state kit on it that animates
    /// it, and whether it is put in it by its tier.
    std::optional<uint32_t> kitAnim;
    bool kitAnimTiered = true;
    /// +0x9f8: the stand state the unit's animation was last given for
    /// (0x0073ac30, 0x0073f060) - the one it stands up, sits down or lies
    /// down from. The renderer keeps it.
    uint8_t standStateSeen = 0;
    /// 0x007222a0 and 0x0071b6b0: whether it may loot, and loots - for
    /// another unit UNIT_FIELD_FLAGS 0x10000000 clear and 0x400 set.
    bool canLoot = false;
    bool looting = false;
    /// +0xa38 0x200: an auto-repeating ranged spell going (Auto Shot,
    /// Shoot; 0x007fa2e0 for AttributesEx2 0x20), and the ranged weapon's
    /// subclass (vtable +300 slot 2), for the ranged readies (0x00714f90).
    bool autoRepeat = false;
    std::optional<uint32_t> rangedSubClass;
    /// Its spline flies (0x004f5290: spline Flags 0x2000), or falls (0x200)
    /// or arcs (0x800; 0x006e9a70, 0x006e9aa0).
    bool splineFlying = false;
    bool splineFalling = false;
    /// 0x00724060: the AnimID of a kit on it whose animation is a death,
    /// and the kit's Flags - such a unit is dead to it (0x0071f560).
    std::optional<uint32_t> deathKitAnim;
    uint32_t deathKitFlags = 0;
    /// 0x00756f40: a vehicle whose VehicleFlags have 0x10000 - while it
    /// casts, only its cast loop is picked for it.
    bool vehicleCastOnly = false;
    /// 0x0071afb0: in combat (UNIT_FIELD_FLAGS 0x800) or with a melee
    /// target - a dance then ends (0x0073b510).
    bool inCombat = false;
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
    /// A swing asked while a swing plays: the one playing - the upper
    /// body's, else the body's - runs on at twice the speed (0x00735cc0 ->
    /// 0x00827000, 2.0) and the asked one waits for it (+0xb88), which the
    /// base animation gives once it ends (0x0071e0d0). Nothing else changes.
    bool queue = false;
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

/// 0x0071df30: a slot of a hovering unit (0x0071c6c0) neither swimming
/// nor flying whose model would play Stand, StandWound or ReadyUnarmed for
/// it (0x0082ced0 -> 0x00826350) plays Hover instead, unblended.
inline constexpr uint32_t kHover = 193;
[[nodiscard]] bool hoverReplaces(uint32_t modelPlays);
/// Applies it to what 0x007385c0 decided, after the decision and before it
/// is played: `modelPlays` is 0x00826350's id for an animation. What was to
/// start that turns out to be what already plays goes on.
template <class ModelPlays>
void applyHover(const Unit& unit, const Slots& now, Decision& d, const ModelPlays& modelPlays) {
    if (!unit.hovering || (unit.moveFlags & kMoveSwimOrFly) != 0) return;
    const auto hover = [&modelPlays](Slot& slot, Action& action, bool& blend, int32_t playing) {
        if (action != Action::Play || slot.anim < 0 || !hoverReplaces(modelPlays(static_cast<uint32_t>(slot.anim))))
            return;
        slot = Slot{static_cast<int32_t>(kHover), 0.0f, true};
        blend = false;
        if (playing == static_cast<int32_t>(kHover)) action = Action::Keep;
    };
    hover(d.body, d.bodyAction, d.bodyBlend, now.body.anim);
    hover(d.upper, d.upperAction, d.upperBlend, now.upper.anim);
}

/// 0x0071e400 on what 0x007385c0 decided: the body and, where it has one of
/// its own, the upper body play the kit's animation `kit` (as the unit is
/// put in it), unblended - what already plays it goes on.
void applyKitAnimation(const Slots& now, Decision& d, uint32_t kit);
/// The same for a rider (+0x98c): the kit's animation goes to its upper
/// body where it is one of the upper body's and not one its mount plays
/// (0x0071d6b0, 0x0071d800); the body is left in the rider's pose.
void applyRiderKitAnimation(const anim::AnimationDataTable& data, const Slots& now, Decision& d, uint32_t kit);

/// What 0x00736640 plays for a blow taken: StandWound, CombatWound with a
/// melee target, CombatCritical for a critical - on the upper body, or on
/// the whole body (`upper` false).
struct Wound {
    uint32_t anim = 8;
    bool upper = true;
};

/// 0x00736640: the wound for a blow a unit takes, `shown` and `body` what
/// plays (0x00717260, 0x008267e0(-1)); nothing for a dead unit, or one
/// knocked down (0x00722180). The whole body takes it standing still out
/// of a fight - not riding, moving, falling, swimming, flying, hovering,
/// sitting or getting up from the ground - or where the body stands or is
/// ready.
[[nodiscard]] std::optional<Wound> wound(const anim::AnimationDataTable& data, const Unit& unit, bool critical,
                                         int32_t shown, int32_t body);

/// 0x007385c0: where `asked` (0x007176f0's already) goes, and what each
/// slot plays after it.
[[nodiscard]] Decision decide(const anim::AnimationDataTable& data, const Unit& unit, const Slot& asked,
                              const Slots& now, Pass pass = Pass::Asked);

}  // namespace upper_body
}  // namespace rendering
}  // namespace wowee
