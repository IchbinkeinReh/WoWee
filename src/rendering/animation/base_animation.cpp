#include "rendering/animation/base_animation.hpp"

namespace wowee {
namespace rendering {
namespace base_animation {

namespace ub = upper_body;

uint32_t meleeReady(const HandItem* mainHand, const HandItem* offHand) {
    constexpr uint32_t kWeapon = 2;
    const HandItem* weapon = mainHand != nullptr && mainHand->itemClass == kWeapon ? mainHand : offHand;
    if (weapon == nullptr || weapon->itemClass != kWeapon) return 25;  // ReadyUnarmed
    switch (weapon->subClass) {
        case 0: case 4: case 7: case 11: case 13: case 14: case 15: case 20:
            return 26;  // Ready1H
        case 1: case 5: case 8: case 12:
            return offHand != nullptr ? 26u : 27u;  // Ready2H
        case 6: case 10: case 17:
            return 28;  // Ready2HL
        default:
            return 25;
    }
}

namespace {

constexpr uint32_t kStand = 0, kDead = 6, kShuffleLeft = 11, kShuffleRight = 12, kWalkBackwards = 13,
                   kWalk = 4, kRun = 5, kFall = 40, kSwimIdle = 41, kSwim = 42, kSwimLeft = 43, kSwimRight = 44,
                   kSwimBackwards = 45, kSitGroundDown = 96, kSitGround = 97, kSleep = 100, kSitChairLow = 102,
                   kSitChairMed = 103, kSitChairHigh = 104, kKneelLoop = 115, kStealthWalk = 119,
                   kStealthStand = 120, kDrowned = 132, kSprint = 143, kHover = 193, kSubmerged = 202,
                   kSettle = 464, kDeathStart = 466, kDeathEndHold = 472, kBirth = 127, kSubmerge = 201,
                   kEmerge = 224, kSitGroundUp = 98, kSleepDown = 99, kSleepUp = 101, kKneelStart = 114,
                   kKneelEnd = 116, kFly = 135, kLoot = 50, kLootHold = 188, kLootUp = 189, kReadyUnarmed = 25,
                   kLoadBow = 105, kLoadRifle = 106, kHoldBow = 109, kHoldRifle = 110, kHoldThrown = 111,
                   kLoadThrown = 112, kFishingCast = 133, kFishingLoop = 134, kEmoteDance = 69;

uint32_t behaviorOf(const anim::AnimationDataTable& data, int32_t a) {
    return a < 0 ? anim::kNoBehavior : data.behavior(static_cast<uint32_t>(a));
}

// 0x0071dd80: dying, dead, drowning.
bool isDying(uint32_t b) { return b == 1 || b == 131 || b == 466 || b == 467; }
// 0x0071dc20: jumping or falling.
bool isAirborneAnim(uint32_t b) { return (b > 36 && b < 41) || b == 467; }
// 0x0071da20, 0x0071da60, 0x0071daa0: the ranged throws, loads and holds.
bool isRangedHold(uint32_t b) {
    return b == 107 || b == 111 || b == 112 || b == 46 || b == 105 || b == 109 || b == 49 || b == 106 || b == 110;
}
// 0x0071c720: hovering, swimming or flying - no shuffling round there.
bool levitates(const ub::Unit& u) { return u.hovering || (u.moveFlags & ub::kMoveSwimOrFly) != 0; }

constexpr uint32_t kMoveBackward = 0x2, kMoveStrafeLeft = 0x4, kMoveTurnLeft = 0x10, kMoveFallingFar = 0x2000;

}  // namespace

Pick pick(const anim::AnimationDataTable& data, const ub::Unit& unit, const Playing& playing, const Ask& ask,
          const std::function<bool(uint32_t)>& has, const std::function<bool(uint32_t)>& hasShown) {
    const uint32_t move = unit.moveFlags;
    const uint32_t shownB = behaviorOf(data, playing.shown);
    const auto keep = [] { return Pick{}; };
    const auto loop = [](uint32_t a) { return Pick{a, true}; };
    const auto once = [](uint32_t a) { return Pick{a, false}; };
    const bool bodyFree = (ask.free & kBodyFree) != 0;
    const uint8_t seen = unit.standStateSeen;

    // 0x00724060: dead - its health gone, feigning death, lying dead, or a
    // kit on it whose animation is a death, which it then plays.
    if (!unit.alive || unit.feignDeath || unit.standState == 7 || unit.deathKitAnim) {
        if (unit.deathKitAnim) {
            const uint32_t k = unit.deathKitFlags;
            const uint32_t flags = ((k & 0x4) ? 0x2u : 0u) | ((k & 0x8) ? 0x4u : 0u) | ((k & 0x10) ? 0x8u : 0u) |
                                   ((k & 0x100) ? 0x20u : 0u);
            return Pick{*unit.deathKitAnim, !isDying(data.behavior(*unit.deathKitAnim)), flags};
        }
        if (hasShown(kDeathStart)) {
            const bool falling = (move & ub::kMoveFalling) != 0 || unit.splineFalling;
            return falling ? once(kDeathStart) : loop(kDeathEndHold);
        }
        // Falling far it is left as it is; dying it goes on.
        if ((move & ub::kMoveFalling) != 0 && (move & kMoveFallingFar) != 0) return keep();
        if (isDying(shownB)) return keep();
        return loop((move & ub::kMoveSwimming) != 0 ? kDrowned : kDead);
    }
    // 0x00716fd0: out of the custom stand state (10), Birth where the model
    // has it.
    if (bodyFree && !unit.riding && seen == 10 && has(kBirth)) return once(kBirth);
    // 0x00756f40: a vehicle that casts as a whole (VehicleFlags 0x10000)
    // has only its cast loop picked while it casts.
    const bool casting = unit.castTimed || unit.channeling;
    const auto castLoop = [&]() -> std::optional<Pick> {
        // 0x00724330: casting (+0xa38 0x400) or channelling, not riding,
        // with a kit loop (0x007224d0) - kept where the upper body has it,
        // and where the body has it only asked again from a body's end with
        // the kit's Flags 0x40.
        if (!casting || (ask.free & (kUpperFree | kBodyFree)) == 0 || unit.riding || !unit.castLoopAnim)
            return std::nullopt;
        const uint32_t k = unit.castLoopKitFlags;
        const uint32_t flags = ((k & 0x4) ? 0x2u : 0u) | ((k & 0x8) ? 0x4u : 0u) | ((k & 0x10) ? 0x8u : 0u) |
                               ((k & 0x100) ? 0x20u : 0u);
        const auto anim = static_cast<int32_t>(*unit.castLoopAnim);
        if (anim == playing.shown && playing.shown == playing.body && ((ask.flags & 0x10) == 0 || (k & 0x40) == 0))
            return Pick{kKeep, true, flags};
        return Pick{*unit.castLoopAnim, true, flags};
    };
    if (unit.vehicleCastOnly && casting) return castLoop().value_or(keep());
    // 0x0071dff0: going under (stand state 9) or coming up from it, its
    // body free and not riding: Submerge, Submerged; Emerge, else Birth.
    if (bodyFree && !unit.riding) {
        std::optional<uint32_t> under;
        if (unit.standState == 9) under = seen == 9 ? kSubmerged : kSubmerge;
        else if (seen == 9) under = has(kEmerge) ? kEmerge : kBirth;
        if (under && has(*under)) {
            if (playing.shown == static_cast<int32_t>(*under)) return keep();
            return *under == kSubmerged ? loop(*under) : once(*under);
        }
    }
    // 0x00724200: in the air - the jump or the fall goes on, else Fall.
    if (unit.leaping) return isAirborneAnim(behaviorOf(data, playing.body)) ? keep() : loop(kFall);
    // 0x00717050: moving.
    if ((move & ub::kMoveDirection) != 0) {
        if ((ask.free & kAllFree) == 0) return keep();
        if ((move & ub::kMoveSwimOrFly) != 0) {
            if ((move & ub::kMoveStrafe) != 0) return loop((move & kMoveStrafeLeft) != 0 ? kSwimLeft : kSwimRight);
            return loop((move & kMoveBackward) != 0 ? kSwimBackwards : kSwim);
        }
        if (unit.splineFlying) return loop(kFly);  // 0x004f5290
        if ((move & kMoveBackward) != 0) return loop(kWalkBackwards);
        if (unit.stealthed) return loop(kStealthWalk);
        if (unit.speed > 11.0f) return loop(kSprint);
        if (unit.speed > unit.walkSpeed + unit.walkSpeed) return loop(kRun);
        return loop(kWalk);
    }
    // 0x00724280: looting - Loot, then held (0x0073b510); LootUp as it
    // stops.
    if (bodyFree && !unit.riding && unit.canLoot) {
        const bool lootPlaying = playing.shown == static_cast<int32_t>(kLoot);
        const bool holdPlaying = playing.shown == static_cast<int32_t>(kLootHold);
        if (unit.looting) return lootPlaying || holdPlaying ? keep() : once(kLoot);
        if (holdPlaying) return once(kLootUp);
    }
    if (const auto p = castLoop()) return *p;
    // 0x0071e0d0: a melee target - the swing that waited, else the swim
    // idle, else the ready for its weapons.
    if ((ask.free & (kUpperFree | kBodyFree)) != 0 && unit.autoAttacking) {
        if ((ask.flags & 0x40) != 0 && ub::isAttack(shownB)) return Pick{static_cast<uint32_t>(playing.shown), false};
        if (ask.queuedAttack != -1) return Pick{static_cast<uint32_t>(ask.queuedAttack), false};
        return loop((move & ub::kMoveSwimOrFly) != 0 ? kSwimIdle : unit.meleeReady);
    }
    // 0x0071e180 (0x0071de90): turning on the spot, on the ground, with no
    // emote, cast or ranged throw playing - the shuffle its way.
    if (((move & ub::kMoveTurn) != 0 || unit.turningInPlace) && !levitates(unit) && !ub::isEmote(shownB) &&
        !ub::isSpell(shownB) && !isRangedHold(shownB)) {
        if ((ask.free & kAllFree) == 0) return keep();
        return loop((move & kMoveTurnLeft) == 0 && !unit.turningLeft ? kShuffleRight : kShuffleLeft);
    }
    // 0x00714f90: the ranged weapon out with an auto-repeating shot: its
    // load (held after, 0x0073b510), the wand's hold, else ReadyUnarmed.
    if (unit.rangedDrawn && unit.autoRepeat) {
        if (!unit.rangedSubClass) return loop(kReadyUnarmed);
        switch (*unit.rangedSubClass) {
            case 2: return once(kLoadBow);
            case 3: case 18: return once(kLoadRifle);
            case 16: return once(kLoadThrown);
            case 19: return loop(kHoldThrown);
            default: return loop(kReadyUnarmed);
        }
    }
    // 0x0071e1f0: the stand state's, from the one it was in (+0xa38 +0x9f8)
    // - its going down, its loop, its getting up - the body free and not
    // riding.
    if (bodyFree && !unit.riding) {
        switch (unit.standState) {
            case 0:
                if (seen == 1) return once(kSitGroundUp);
                if (seen == 3) return once(kSleepUp);
                if (seen == 8) return once(kKneelEnd);
                break;
            case 1:
                if (seen == 0 || playing.shown == static_cast<int32_t>(kSitGroundDown)) return once(kSitGroundDown);
                return loop(kSitGround);
            case 3: return seen != 0 ? loop(kSleep) : once(kSleepDown);
            case 4: return loop(kSitChairLow);
            case 5: return loop(kSitChairMed);
            case 6: return loop(kSitChairHigh);
            case 8: return seen != 0 ? loop(kKneelLoop) : once(kKneelStart);
            default: break;
        }
    }
    // 0x007171c0: the emote state's animation.
    if (unit.emoteStateAnim && ((bodyFree && !unit.riding) || (ask.free & kUpperFree) != 0))
        return loop(*unit.emoteStateAnim);
    // 0x0071e340: nothing else - on a seat's ride the seat's; a settle goes
    // on; else the swim idle, the stealth stand, the hover or the stand.
    if (unit.seatedRide) return keep();
    if (shownB == kSettle) return loop(static_cast<uint32_t>(playing.shown));
    if ((move & ub::kMoveSwimOrFly) != 0) return loop(kSwimIdle);
    if (unit.stealthed) return loop(kStealthStand);
    if (unit.hovering) return loop(kHover);
    return loop(kStand);
}

std::optional<Pick> afterOneShot(const anim::AnimationDataTable& data, const ub::Unit& unit, uint32_t ended) {
    const uint32_t st = unit.standState;
    switch (data.behavior(ended)) {
        case kSitGroundDown:
        case kSitGround:
            return st != 1 ? Pick{kSitGroundUp, false} : Pick{kSitGround, true};
        case kSleepDown:
        case kSleep:
            return st != 3 ? Pick{kSleepUp, false} : Pick{kSleep, true};
        case kSitChairLow:
            if (st == 4) return Pick{kSitChairLow, true};
            break;
        case kSitChairMed:
            if (st == 5) return Pick{kSitChairMed, true};
            break;
        case kSitChairHigh:
            if (st == 6) return Pick{kSitChairHigh, true};
            break;
        case kKneelStart:
        case kKneelLoop:
            return st != 8 ? Pick{kKneelEnd, false} : Pick{kKneelLoop, true};
        case kLoadBow: return Pick{kHoldBow, true};
        case kLoadRifle: return Pick{kHoldRifle, true};
        case kLoot: return Pick{kLootHold, true};
        case kSubmerge: return Pick{kSubmerged, true};
        case kFishingCast: return Pick{kFishingLoop, true};
        case kEmoteDance:
            // Danced on out of a fight (0x0071afb0).
            if (!unit.inCombat) return Pick{kEmoteDance, true};
            break;
        default:
            break;
    }
    return std::nullopt;
}

}  // namespace base_animation
}  // namespace rendering
}  // namespace wowee
