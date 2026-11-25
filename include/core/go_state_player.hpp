#pragma once

#include "game/go_interaction_anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace wowee::core {

/// Puts a game object's model in a state's animation (0x0070d1e0): a transition plays once,
/// a pose goes round - 0x0070d7e0 enters it again at each end. The model is asked for the
/// state's own animation and settles itself what it plays for one it lacks, and how
/// (0x00826350): Closed on a chest with Close alone is Close's last frame, Close on a door
/// with Open alone is Open backwards.
template <class Model>
void playGameObjectState(Model& m2, uint32_t instanceId, uint8_t state) {
    namespace gi = game::go_interaction;
    const auto ask = gi::stateAnimation(state, [&](uint32_t anim) { return m2.hasAnimation(instanceId, anim); });
    if (gi::stateAfter(state)) {
        m2.setInstanceAnimationHeld(instanceId, ask.anim, false);
    } else {
        m2.setInstanceAnimation(instanceId, ask.anim, true);
    }
    if (ask.held) m2.setInstanceAnimationFrozen(instanceId, true);
}

/// What is left, in ms, of a sequence at `timeMs` of `durationMs` (0x0070d1e0's +0x18); none
/// of one that stands.
[[nodiscard]] inline float sequenceLeftMs(float timeMs, float durationMs, float speed) {
    if (speed == 0.0f) return 0.0f;
    const float left = speed > 0.0f ? durationMs - timeMs : timeMs;
    return std::max(left, 0.0f) / std::fabs(speed);
}

}  // namespace wowee::core
