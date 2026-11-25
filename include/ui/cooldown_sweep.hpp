#pragma once

// The Cooldown frame (CGCooldown, vtable 0x00a1d100): the shade that goes
// round an icon, the star at its end and the line along its edge.
//
// The frame's own update (0x005ec8f0) runs while it is visible. The shade is
// there until the time is up; then, unless the frame is reversed, the same
// frame is the star for one second, and then it hides itself (0x0048f620).
// A reversed one stays at its last millisecond, all of it shaded.
//
// Pure arithmetic, in fractions of the frame's rect with v growing downward.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace wowee::ui::cooldown_sweep {

/// What SetCooldown (0x005ece80) and the update keep.
struct State {
    uint32_t startMs = 0;
    uint32_t durationMs = 0;
    /// The star's second is on (+0x2a8).
    bool flashing = false;
    /// SetCooldown has been called: its two pictures are there (+0x2b0,
    /// +0x3d4), and without them nothing is drawn (0x005ebd20).
    bool drawn = false;
};

inline constexpr uint32_t kFlashMs = 1000;

/// SetCooldown's numbers: seconds, rounded to milliseconds (0x005ece80).
inline uint32_t msOf(double seconds) { return static_cast<uint32_t>(std::llround(seconds * 1000.0)); }

inline void set(State& s, double start, double duration) {
    s.startMs = msOf(start);
    s.durationMs = msOf(duration);
    s.flashing = false;
    s.drawn = true;
}

/// One update of a visible frame (0x005ec8f0). True when the frame hides.
inline bool update(State& s, bool reverse, uint32_t nowMs) {
    if (nowMs - s.startMs < s.durationMs) return false;
    if (s.flashing) {
        s.flashing = false;
        return true;
    }
    if (reverse) return false;
    s.flashing = true;
    s.startMs = nowMs;
    s.durationMs = kFlashMs;
    return false;
}

/// How far round it is, 0 to 1. Past the end a reversed frame is at its last
/// millisecond, and any other has nothing to draw (negative).
inline float progress(const State& s, bool reverse, uint32_t nowMs) {
    uint32_t elapsed = nowMs - s.startMs;
    if (elapsed >= s.durationMs) {
        if (!reverse || s.durationMs == 0) return -1.0f;
        elapsed = s.durationMs - 1;
    }
    return static_cast<float>(elapsed) / static_cast<float>(s.durationMs);
}

struct Point {
    float u = 0.0f, v = 0.0f;
};

/// The corners and middles the edge runs along, clockwise from the top
/// middle: an eighth of the time from each to the next (0x005eb130,
/// 0x005eb3a0, 0x005eb670, 0x005eb9a0).
inline constexpr std::array<Point, 9> kRound = {
    {{0.5f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f}, {1.0f, 1.0f}, {0.5f, 1.0f},
     {0.0f, 1.0f}, {0.0f, 0.5f}, {0.0f, 0.0f}, {0.5f, 0.0f}}};

/// The rim of the shade, in order; with the middle it is a fan. What is left
/// to go is shaded, or with `reverse` what has passed. Answers how many.
inline int rim(float progress, bool reverse, std::array<Point, 10>& out) {
    const float eighths = std::clamp(progress, 0.0f, 1.0f) * 8.0f;
    const int at = std::min(static_cast<int>(eighths), 7);
    const float f = eighths - static_cast<float>(at);
    const Point a = kRound[at], b = kRound[at + 1];
    const Point edge{a.u + (b.u - a.u) * f, a.v + (b.v - a.v) * f};
    int n = 0;
    if (reverse) {
        for (int i = 0; i <= at; ++i) out[n++] = kRound[i];
        out[n++] = edge;
    } else {
        out[n++] = edge;
        for (int i = at + 1; i <= 8; ++i) out[n++] = kRound[i];
    }
    return n;
}

/// Where a picture turned by `angle`, of which `scale` times as much is
/// shown, is looked up at the corners: top left, top right, bottom left,
/// bottom right.
inline std::array<Point, 4> turned(float angle, float scale) {
    const float c = std::cos(angle) * 0.5f, s = std::sin(angle) * 0.5f;
    return {{{(-c + s) * scale + 0.5f, (-s - c) * scale + 0.5f},
             {(c + s) * scale + 0.5f, (s - c) * scale + 0.5f},
             {(-c - s) * scale + 0.5f, (c - s) * scale + 0.5f},
             {(c - s) * scale + 0.5f, (s + c) * scale + 0.5f}}};
}

/// How far the line along the edge is turned with it.
inline float edgeAngle(float progress) { return progress * -6.2831855f; }

struct Flash {
    float angle = 0.0f, scale = 1.0f, alpha = 0.0f;
};

/// The star's second (0x005ec1e0): six steps between seven values each of its
/// angle (0x00c24254), how much of the picture is shown (0x00ad1418) and its
/// alpha (0x00ad1434).
inline Flash flash(uint32_t elapsedMs) {
    constexpr std::array<float, 7> kAngle = {0.0f,         -0.13037610f, -0.25918141f, -0.39269909f,
                                             -0.51836282f, -0.65188050f, -0.78539819f};
    constexpr std::array<float, 7> kScale = {1.0f,        0.69930071f, 0.54054052f, 0.62893081f,
                                             0.75187969f, 0.625f,      0.86956525f};
    constexpr std::array<float, 7> kAlpha = {0.0f, 0.5f, 1.0f, 0.75f, 0.5f, 0.25f, 0.0f};
    const float step = static_cast<float>(elapsedMs) / 166.66667f;
    const int i = std::clamp(static_cast<int>(step), 0, 5);
    const float f = step - static_cast<float>(i);
    const auto mix = [&](const std::array<float, 7>& t) { return t[i] + (t[i + 1] - t[i]) * f; };
    return {mix(kAngle), mix(kScale), mix(kAlpha)};
}

}  // namespace wowee::ui::cooldown_sweep
