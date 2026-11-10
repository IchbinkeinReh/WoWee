#pragma once

// A ship's sway on its route (TransportPhysics.dbc, the row its MO_TRANSPORT
// template's sixth datum names, 0x00711b50): a wave lifting the hull, a roll
// and a pitch rocking it, the roll leaning into a turn, all of it damped while
// the ship is slow or still (0x007f7dd0 -> 0x007f7b30). The client draws the
// ship with yaw, then pitch, then roll in its own frame (0x007134a0), and its
// passengers ride that frame.

#include <cmath>
#include <cstdint>

namespace wowee::game::transport_physics {

/// A TransportPhysics row's ten values (+4..+0x28).
struct Physics {
    float waveAmp = 0.0f;
    float waveTimeScale = 0.0f;
    float rollAmp = 0.0f;
    float rollTimeScale = 0.0f;
    float pitchAmp = 0.0f;
    float pitchTimeScale = 0.0f;
    float maxBank = 0.0f;
    float maxBankTurnSpeed = 0.0f;
    float speedDampThresh = 0.0f;
    float speedDamp = 0.0f;
};

/// 0x007f7dd0's speed: none below 15% of the damping threshold, eased in up
/// to 30% of it.
inline float effectiveSpeed(const Physics& p, float speed) {
    if (!(speed > 0.0f)) return speed;
    const float lo = p.speedDampThresh * 0.15f;
    const float hi = p.speedDampThresh * 0.3f;
    if (speed < lo) return 0.0f;
    if (speed < hi) {
        const float f = (speed - lo) / (hi - lo);
        return f * f * speed;
    }
    return speed;
}

/// 0x007f7dd0's turn rate from the heading now and at four places behind,
/// 0.45 s of travel apart: the summed change, per second, over five.
inline float turnRate(const float (&headings)[5]) {
    constexpr float kPi = 3.1415927f;
    float acc = 0.0f;
    float prev = headings[0];
    for (int k = 1; k < 5; ++k) {
        float diff = prev - headings[k];
        if (diff > kPi) diff -= 6.2831855f;
        else if (diff < -kPi) diff += 6.2831855f;
        acc += diff * 2.2222223f;
        prev = headings[k];
    }
    return acc * 0.2f;
}

struct Sway {
    float lift = 0.0f;
    float roll = 0.0f;
    float pitch = 0.0f;
};

namespace detail {
/// sin of the share of a period `timeScale` makes of the time, plus `phase`.
inline float wave(uint32_t timeMs, float timeScale, float phase) {
    const auto period = static_cast<uint32_t>(std::lround(6283.1855f / timeScale));
    if (period == 0) return 0.0f;
    const float share = static_cast<float>(timeMs % period) * 6.2831855f / static_cast<float>(period);
    return std::sin(share + phase);
}
}  // namespace detail

/// 0x007f7b30 at route time `timeMs`, the ship making `speed` (already
/// through effectiveSpeed) and turning at `turn` radians a second.
inline Sway sway(const Physics& p, uint32_t timeMs, float speed, float turn) {
    constexpr float kTiny = 2.3841858e-07f;
    float damp = 1.0f;
    if (speed < p.speedDampThresh) {
        const float s = std::sqrt(speed / p.speedDampThresh);
        damp = (1.0f - p.speedDamp) * s * s + p.speedDamp;
    }
    Sway out;
    if (std::fabs(p.waveTimeScale) >= kTiny)
        out.lift = detail::wave(timeMs, p.waveTimeScale, 0.0f) * p.waveAmp * damp;
    if (turn > p.maxBankTurnSpeed) turn = p.maxBankTurnSpeed;
    else if (turn < -p.maxBankTurnSpeed) turn = -p.maxBankTurnSpeed;
    if (std::fabs(p.rollTimeScale) >= kTiny) {
        out.roll = (detail::wave(timeMs, p.rollTimeScale, 0.5f) * p.rollAmp +
                    (p.maxBank / p.maxBankTurnSpeed) * turn) * damp;
    }
    if (std::fabs(p.pitchTimeScale) >= kTiny)
        out.pitch = detail::wave(timeMs, p.pitchTimeScale, 0.2f) * p.pitchAmp * damp;
    return out;
}

}  // namespace wowee::game::transport_physics
