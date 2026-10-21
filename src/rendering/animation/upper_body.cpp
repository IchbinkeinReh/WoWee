#include "rendering/animation/upper_body.hpp"
#include "rendering/m2_track_sampler.hpp"

namespace wowee {
namespace rendering {
namespace upper_body {

bool isMovement(uint32_t anim) { return m2_track::isLocomotionAnimation(anim); }

bool isEmote(uint32_t b) {
    return (b >= 60 && b <= 74) || (b >= 76 && b <= 78) || (b >= 80 && b <= 84) || b == 113 ||
           (b >= 136 && b <= 138) || b == 178 || b == 185 || b == 186 || b == 195;
}

bool isAttack(uint32_t b) {
    return b == 10 || (b >= 16 && b <= 24) || b == 30 || b == 36 || (b >= 57 && b <= 59) || (b >= 85 && b <= 88) ||
           b == 95 || b == 117 || b == 118 || (b >= 170 && b <= 179) || b == 212;
}

bool isSpell(uint32_t b) { return b == 2 || b == 32 || b == 33 || b == 53 || b == 54; }

bool isReady(uint32_t b) { return b >= 25 && b <= 29; }

bool isReadySpell(uint32_t b) { return b == 51 || b == 52; }

bool isJump(uint32_t b) { return (b >= 37 && b <= 39) || b == 187; }

bool isLanding(uint32_t b) { return b == 39 || b == 187; }

bool isFishing(uint32_t b) { return b == 133 || b == 134; }

bool isDeath(uint32_t b) {
    return b == 1 || b == 6 || b == 131 || b == 132 || (b >= 466 && b <= 468) || b == 472;
}

bool isWholeBodySpecial(uint32_t b) { return b == 57 || b == 58 || b == 118; }

bool isUpperBody(uint32_t b) {
    return b == 2 || (b >= 8 && b <= 10) || (b >= 14 && b <= 36) || (b >= 46 && b <= 49) || (b >= 51 && b <= 74) ||
           (b >= 76 && b <= 78) || (b >= 80 && b <= 90) || (b >= 105 && b <= 113) || b == 117 || b == 118 ||
           (b >= 122 && b <= 125) || (b >= 128 && b <= 130) || b == 133 || b == 134 || (b >= 136 && b <= 138) ||
           (b >= 153 && b <= 156) || b == 185 || b == 186 || b == 195 || (b >= 213 && b <= 222) || b == 225;
}

bool isMountAnimation(uint32_t b) {
    return b == 0 || b == 1 || (b >= 3 && b <= 6) || (b >= 8 && b <= 13) || (b >= 37 && b <= 45) ||
           (b >= 92 && b <= 95) || b == 119 || b == 120 || b == 127 || b == 131 || b == 132 || b == 135 ||
           b == 143 || b == 187 || b == 193;
}

namespace {
// 0x0071dd30: a swing or a combat ready.
bool isAttackOrReady(uint32_t b) { return isAttack(b) || isReady(b); }
// 0x0071dc70: a cast or a spell ready.
bool isSpellOrReady(uint32_t b) { return isSpell(b) || isReadySpell(b); }
// 0x00715880: standing, sitting or in a chair.
bool fishesFrom(uint8_t standState) {
    return standState == 0 || standState == 1 || (standState >= 4 && standState <= 6);
}
uint32_t behaviorOf(const anim::AnimationDataTable& data, int32_t anim) {
    return anim < 0 ? anim::kNoBehavior : data.behavior(static_cast<uint32_t>(anim));
}
}  // namespace

bool splits(const anim::AnimationDataTable& data, const Unit& unit, uint32_t asked, int32_t current) {
    if (current < 0) return false;
    const uint32_t b = data.behavior(asked);
    if (isDeath(b)) return false;
    const uint32_t move = unit.moveFlags;
    const bool turning = (move & kMoveTurn) != 0 || unit.turningInPlace;
    bool split = false;
    // An upper-body animation on the move, turning, leaping, sitting, or a
    // ready on a mount.
    if (isUpperBody(b) && (unit.turningInPlace || (move & kMoveAny) != 0 || unit.leaping || unit.standState != 0 ||
                           (isReady(b) && unit.mounted))) {
        split = true;
    }
    // A jump over a swing or a ready; a swing falling; anything casting on
    // the move.
    if (!split && isJump(b) && isAttackOrReady(behaviorOf(data, current))) split = true;
    if (!split && isAttack(b) && (move & kMoveFalling) != 0) split = true;
    if (!split && unit.castTimed && (move & kMoveTravel) != 0) split = true;
    // Never a landing, nor fishing anywhere but standing or sitting.
    if (isLanding(b)) split = false;
    if (isFishing(b) && !fishesFrom(unit.standState)) split = false;
    // Casting, channelling or swinging while turning, unless strafing.
    if ((unit.castTimed || unit.channeling || unit.autoAttacking) && turning && (move & kMoveStrafe) == 0)
        split = false;
    return split;
}

Decision decide(const anim::AnimationDataTable& data, const Unit& unit, const Slot& asked, const Slots& now,
                Pass pass) {
    Decision d;
    d.body = now.body;
    d.upper = now.upper;
    // +0xa38 0x20 and 0x40: the upper body may change, the body may. When
    // the body's one-shot has ended only the body is asked (0x0073bbd0).
    bool upperChanges = pass == Pass::Asked;
    bool bodyChanges = true;
    const uint32_t want = static_cast<uint32_t>(asked.anim);
    const uint32_t b = data.behavior(want);
    const bool hasUpper = now.upper.anim >= 0;
    const int32_t current = hasUpper ? now.upper.anim : now.body.anim;
    bool split = upperChanges && splits(data, unit, want, current);
    // Death, the whole-body specials, and a model that moves as a whole go
    // to the body alone.
    const bool wholeBody = isWholeBodySpecial(b) || isDeath(b) || unit.wholeBodyOnly;
    if (wholeBody) {
        split = false;
        upperChanges = false;
    }
    bool bodyIsAsked = false;

    if (!unit.riding) {
        bool toUpper = false;
        bool moveOrStand = false;
        bool bodyToUpper = false;
        if (want == 61) {
            // EmoteEat goes to the upper body whatever the unit does.
            toUpper = true;
        } else if (want == 62 && (unit.moveFlags & kMoveSwimOrFly) != 0) {
            // EmoteWork swimming or flying: the upper body, the body as it is.
            toUpper = true;
            bodyChanges = false;
        } else if (split) {
            const uint32_t cb = behaviorOf(data, current);
            if (isEmote(b) && want != 69) {
                toUpper = true;
            } else if (!unit.castTimed && !isAttackOrReady(cb) && !isSpellOrReady(cb) && !unit.rangedDrawn) {
                toUpper = true;
            } else if (!isMovement(want) && !isReady(b) && want != 0) {
                toUpper = true;
                // Swimming or flying in place, the body idles in the water.
                if ((unit.moveFlags & kMoveSwimOrFly) != 0 && (unit.moveFlags & kMoveDirection) == 0)
                    d.body = Slot{static_cast<int32_t>(unit.swimIdle), 0.0f, true};
            } else {
                moveOrStand = true;
            }
        } else {
            const uint32_t bodyB = behaviorOf(data, now.body.anim);
            const uint32_t cb = behaviorOf(data, current);
            if (isMovement(want) && (isAttack(bodyB) || isSpell(bodyB))) {
                // Moving off mid-swing or mid-cast: it goes on above.
                bodyToUpper = true;
            } else if (hasUpper && ((isAttackOrReady(cb) && (isReady(b) || want == 0)) ||
                                    (isSpellOrReady(cb) && (isReadySpell(b) || isReady(b) || want == 0)))) {
                // Stopping mid-swing or mid-cast: the whole body takes it.
                upperChanges = true;
                d.body = now.upper;
                d.bodyFromUpper = true;
                d.upper = Slot{};
            } else {
                d.body = asked;
                bodyIsAsked = true;
            }
        }
        if (moveOrStand && !hasUpper) bodyToUpper = true;
        if (bodyToUpper) {
            upperChanges = true;
            d.upper = now.body;
            d.upperFromBody = true;
        }
        if (moveOrStand || bodyToUpper) {
            d.body = asked;
            bodyIsAsked = true;
        }
        if (toUpper) {
            upperChanges = true;
            d.upper = asked;
        }
    } else {
        // A rider: its mount plays what moves, the upper body the rest it
        // can, and the body holds the rider's pose.
        if (isMountAnimation(b)) d.toMount = true;
        if (!isMountAnimation(b) && isUpperBody(b)) d.upper = asked;
        if (behaviorOf(data, now.body.anim) != unit.riderPose) {
            d.body = Slot{static_cast<int32_t>(unit.riderPoseShown), 0.0f, true};
            d.bodyBlend = false;
        }
    }

    if (wholeBody) {
        upperChanges = false;
        bodyChanges = true;
        d.body = asked;
        bodyIsAsked = true;
    }

    // 0x00737ef0 for each: the same animation goes on, another starts. An
    // upper body taking up what the body plays comes in unblended, and the
    // body blends into what it takes up.
    if (upperChanges) {
        if (d.upper.anim < 0) {
            d.upperAction = hasUpper ? Action::Release : Action::Keep;
        } else {
            if (!hasUpper && d.upper.anim == now.body.anim) {
                d.bodyBlend = true;
                d.upperBlend = false;
            }
            const bool again = d.upper.anim == asked.anim && !asked.loop;
            d.upperAction = d.upper.anim == now.upper.anim && !again ? Action::Keep : Action::Play;
        }
    }
    // Dying, the upper body goes back to the body (0x00832840 on +0xb84).
    if (isDeath(b) && hasUpper) d.upperAction = Action::Release;
    if (bodyChanges) {
        // A one-shot asked again starts again: there is no attack queue
        // here (0x007385c0's first test, +0xb88).
        const bool again = bodyIsAsked && !asked.loop;
        d.bodyAction = d.body.anim == now.body.anim && !again ? Action::Keep : Action::Play;
    }
    return d;
}

}  // namespace upper_body
}  // namespace rendering
}  // namespace wowee
