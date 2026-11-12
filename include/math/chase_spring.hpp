#pragma once

// The client's chase of a value toward its target (0x007160b0): a critically
// damped spring that settles at `rate` without overshooting, its velocity
// kept between steps. The vehicle camera, a unit's drawn facing and its body
// turn all run on it.

namespace wowee::math {

template <class T>
inline void chaseStep(T& value, T& velocity, const T& target, float rate, float dt) {
    const float x = rate * dt;
    const float d = 1.0f / (x * x * 0.48f + x * x * x * 0.235f + x + 1.0f);
    const T off = value - target;
    const T push = (off * rate + velocity) * dt;
    value = (off + push) * d + target;
    velocity = (velocity - push * rate) * d;
}

}  // namespace wowee::math
