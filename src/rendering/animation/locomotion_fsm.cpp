#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <algorithm>

namespace wowee {
namespace rendering {

// ── One-shot completion helper ─────────────────────────────────────────────────────────

bool LocomotionFSM::oneShotComplete(const Input& in, uint32_t expectedAnimId) const {
    if (!in.haveAnimState) return false;
    return in.currentAnimId != expectedAnimId ||
           (in.currentAnimDuration > 0.1f && in.currentAnimTime >= in.currentAnimDuration - 0.05f);
}

// ── Landing ───────────────────────────────────────────────────────────────────

// The client's landing (FUN_0073d2b0), decided as the feet touch down: with no
// direction held it plays JumpEnd; running on - forward or strafing, not
// backing up, faster than twice walk speed (FUN_00716fa0) - JumpLandRun;
// walking or backing up goes straight back to moving. WoWee dropped the
// landing whenever the player was moving, so a jump on the run never landed.
void LocomotionFSM::land(const Input& in, const AnimCapabilitySet& caps) {
    jumpEndSeen_ = false;
    landDecided_ = true;
    landRun_ = in.moving && !in.movingBackward && in.sprinting &&
               caps.resolvedJumpLandRun != 0;
    if (in.moving && !landRun_) {
        state_ = in.sprinting ? State::RUN : State::WALK;
    } else {
        state_ = State::JUMP_END;
    }
}

// ── Event handling ───────────────────────────────────────────────────────────

void LocomotionFSM::onEvent(AnimEvent event) {
    switch (event) {
        case AnimEvent::SWIM_ENTER:
            state_ = State::SWIM_IDLE;
            break;
        case AnimEvent::SWIM_EXIT:
            state_ = State::IDLE;
            break;
        case AnimEvent::JUMP:
            if (state_ != State::SWIM_IDLE && state_ != State::SWIM) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            }
            break;
        case AnimEvent::LANDED:
            if (state_ == State::JUMP_MID || state_ == State::JUMP_START) {
                jumpEndSeen_ = false;
                landDecided_ = false;
                state_ = State::JUMP_END;
            }
            break;
        case AnimEvent::MOVE_START:
            graceTimer_ = kGraceSec;
            break;
        case AnimEvent::MOVE_STOP:
            // Grace timer handles the delay in updateTransitions
            break;
        default:
            break;
    }
}

// ── State transitions ────────────────────────────────────────────────────────

void LocomotionFSM::updateTransitions(const Input& in, const AnimCapabilitySet& caps) {
    // Update grace timer
    if (in.moving) {
        graceTimer_ = kGraceSec;
        wasSprinting_ = in.sprinting;
    } else {
        graceTimer_ = std::max(0.0f, graceTimer_ - in.deltaTime);
    }

    const bool effectiveMoving = in.moving || graceTimer_ > 0.0f;
    const bool effectiveSprinting = in.sprinting || (!in.moving && effectiveMoving && wasSprinting_);

    switch (state_) {
        case State::IDLE:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (effectiveMoving && effectiveSprinting) {
                state_ = State::RUN;
            } else if (effectiveMoving) {
                state_ = State::WALK;
            }
            break;

        case State::WALK:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (!effectiveMoving) {
                state_ = State::IDLE;
            } else if (effectiveSprinting) {
                state_ = State::RUN;
            }
            break;

        case State::RUN:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (!effectiveMoving) {
                state_ = State::IDLE;
            } else if (!effectiveSprinting) {
                state_ = State::WALK;
            }
            break;

        case State::JUMP_START:
            if (in.swimming) {
                state_ = State::SWIM_IDLE;
            } else if (in.grounded) {
                land(in, caps);
            } else if (caps.resolvedJumpStart == 0) {
                // Model doesn't have JUMP_START animation - skip to mid-air
                state_ = State::JUMP_MID;
            } else if (in.haveAnimState) {
                // Use the same resolved ID that resolve() outputs
                uint32_t expected = caps.resolvedJumpStart;
                if (in.currentAnimId == expected) jumpStartSeen_ = true;
                // Also detect completion via renderer's auto-STAND reset:
                // once the animation was seen and currentAnimId changed, it completed.
                if (jumpStartSeen_ && oneShotComplete(in, expected)) {
                    state_ = State::JUMP_MID;
                }
            } else {
                // No animation state available - fall through after 1 frame
                state_ = State::JUMP_MID;
            }
            break;

        case State::JUMP_MID:
            if (in.swimming) {
                state_ = State::SWIM_IDLE;
            } else if (in.grounded) {
                land(in, caps);
            }
            break;

        case State::JUMP_END: {
            // Reached by the LANDED event, which carries no input to decide by.
            if (!landDecided_) {
                land(in, caps);
                if (state_ != State::JUMP_END) break;
            }
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (landRun_) {
                if (!effectiveMoving || in.movingBackward) {
                    // Stopped or turned back mid-landing: plain locomotion.
                    state_ = !effectiveMoving ? State::IDLE
                           : effectiveSprinting ? State::RUN : State::WALK;
                } else if (in.haveAnimState) {
                    const uint32_t expected = caps.resolvedJumpLandRun;
                    if (in.currentAnimId == expected) jumpEndSeen_ = true;
                    if (jumpEndSeen_ && oneShotComplete(in, expected)) state_ = State::RUN;
                } else {
                    state_ = State::RUN;
                }
            } else if (effectiveMoving) {
                // Walking or backing up: no landing animation, as in the client.
                state_ = effectiveSprinting ? State::RUN : State::WALK;
            } else if (caps.resolvedJumpEnd == 0) {
                // Model doesn't have JUMP_END animation - go straight to IDLE
                state_ = State::IDLE;
            } else if (in.haveAnimState) {
                uint32_t expected = caps.resolvedJumpEnd;
                if (in.currentAnimId == expected) jumpEndSeen_ = true;
                // Only transition to IDLE after landing animation completes
                if (jumpEndSeen_ && oneShotComplete(in, expected)) {
                    state_ = State::IDLE;
                }
            } else {
                state_ = State::IDLE;
            }
            break;
        }

        case State::SWIM_IDLE:
            if (!in.swimming) {
                state_ = effectiveMoving ? State::WALK : State::IDLE;
            } else if (effectiveMoving) {
                state_ = State::SWIM;
            }
            break;

        case State::SWIM:
            if (!in.swimming) {
                state_ = effectiveMoving ? State::WALK : State::IDLE;
            } else if (!effectiveMoving) {
                state_ = State::SWIM_IDLE;
            }
            break;
    }
}

// ── Animation resolution ─────────────────────────────────────────────────────

AnimOutput LocomotionFSM::resolve(const Input& in, const AnimCapabilitySet& caps) {
    updateTransitions(in, caps);

    const bool anyStrafeLeft = in.strafeLeft && !in.strafeRight && !in.movingBackward;
    const bool anyStrafeRight = in.strafeRight && !in.strafeLeft && !in.movingBackward;

    uint32_t animId = anim::STAND;
    bool animSelected = true;
    bool loop = true;

    switch (state_) {
        case State::IDLE:
            animId = anim::STAND;
            break;

        case State::WALK:
            if (in.movingBackward) {
                animId = caps.resolvedWalkBackwards ? caps.resolvedWalkBackwards
                       : caps.resolvedWalk           ? caps.resolvedWalk
                       :                               anim::WALK_BACKWARDS;
            } else {
                animId = caps.resolvedWalk ? caps.resolvedWalk : anim::WALK;
            }
            break;

        case State::RUN:
            if (in.movingBackward) {
                animId = caps.resolvedWalkBackwards ? caps.resolvedWalkBackwards
                       : caps.resolvedWalk           ? caps.resolvedWalk
                       :                               anim::WALK_BACKWARDS;
            } else if (in.sprintAura) {
                animId = caps.resolvedSprint ? caps.resolvedSprint : anim::RUN;
            } else {
                animId = caps.resolvedRun ? caps.resolvedRun : anim::RUN;
            }
            break;

        case State::JUMP_START:
            animId = caps.resolvedJumpStart ? caps.resolvedJumpStart : anim::JUMP_START;
            loop = false;
            break;
        case State::JUMP_MID:
            animId = caps.resolvedJump ? caps.resolvedJump : anim::JUMP;
            loop = true;  // Must loop - long falls outlast a single play cycle
            break;
        case State::JUMP_END:
            animId = landRun_ ? caps.resolvedJumpLandRun
                   : caps.resolvedJumpEnd ? caps.resolvedJumpEnd : anim::JUMP_END;
            loop = false;
            break;

        case State::SWIM_IDLE:
            animId = caps.resolvedSwimIdle ? caps.resolvedSwimIdle : anim::SWIM_IDLE;
            break;

        case State::SWIM:
            if (in.movingBackward) {
                animId = caps.resolvedSwimBackwards ? caps.resolvedSwimBackwards
                       : caps.resolvedSwim           ? caps.resolvedSwim
                       :                               anim::SWIM;
            } else if (anyStrafeLeft) {
                animId = caps.resolvedSwimLeft ? caps.resolvedSwimLeft
                       : caps.resolvedSwim    ? caps.resolvedSwim
                       :                        anim::SWIM;
            } else if (anyStrafeRight) {
                animId = caps.resolvedSwimRight ? caps.resolvedSwimRight
                       : caps.resolvedSwim     ? caps.resolvedSwim
                       :                         anim::SWIM;
            } else {
                animId = caps.resolvedSwim ? caps.resolvedSwim : anim::SWIM;
            }
            break;
    }

    if (!animSelected) return AnimOutput::stay();
    return AnimOutput::ok(animId, loop);
}

// ── Reset ────────────────────────────────────────────────────────────────────

void LocomotionFSM::reset() {
    state_ = State::IDLE;
    graceTimer_ = 0.0f;
    wasSprinting_ = false;
    jumpStartSeen_ = false;
    jumpEndSeen_ = false;
    landDecided_ = false;
    landRun_ = false;
}

} // namespace rendering
} // namespace wowee
