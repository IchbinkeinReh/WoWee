#pragma once

/// The in-game time of day, the way Wow.exe 3.3.5a (build 12340) keeps it.
///
/// The server sends a time and a speed once (SMSG_LOGIN_SETTIMESPEED, and
/// again with SMSG_GAMESPEED_SET / SMSG_GAMETIME_SET) and the client runs the
/// clock on from there itself. It was stored and never advanced, so the sky,
/// the sun and the fog stayed at the login hour for the whole session.
///
/// Client: 0x0076cff0 computes
///     minutes = base + elapsed_ms * 0.001 * speed,  wrapped at 1440,
/// and returns minutes / 1440. `speed` is game minutes per real second, set
/// through 0x0076cfa0, which clamps it to [1/60, 60] and falls back to 1/60
/// (0x3c888889) for anything below the range. 1/60 - what servers send - is a
/// game day per real day, not per real hour.

#include <cmath>

namespace wowee::game {

/// The client's default and lower bound for the clock's speed: one game
/// minute per sixty real seconds (0x0076cfa0's 0x3c888889).
inline constexpr float kDefaultGameTimeSpeed = 1.0f / 60.0f;

/// The speed the clock runs at for a speed the server sent (0x0076cfa0).
/// Below 1/60, or not a number, is the default; above 60 is 60.
inline float clampGameTimeSpeed(float speed) {
    if (!(speed >= kDefaultGameTimeSpeed)) return kDefaultGameTimeSpeed;
    return speed > 60.0f ? 60.0f : speed;
}

/// The time of day, in hours since midnight, `elapsedSeconds` of real time
/// after the server said it was `baseHours` (0x0076cff0).
inline float advanceGameClockHours(float baseHours, double elapsedSeconds, float speed) {
    if (elapsedSeconds < 0.0) elapsedSeconds = 0.0;
    double minutes = static_cast<double>(baseHours) * 60.0 +
                     elapsedSeconds * static_cast<double>(clampGameTimeSpeed(speed));
    minutes = std::fmod(minutes, 1440.0);
    if (minutes < 0.0) minutes += 1440.0;
    return static_cast<float>(minutes / 60.0);
}

}  // namespace wowee::game
