#pragma once

/// A Cooldown frame's sweep and the flash at its end (PaperDollInfoFrame.cpp's
/// neighbour 0x005ecd70 - 0x005ec8f0).

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace wowee::ui::cooldown {

/// SetCooldown's start and duration, in seconds, and whether what runs is the
/// flash after it.
struct Clock {
    double start = 0.0;
    double duration = 0.0;
    bool flash = false;

    [[nodiscard]] bool runs() const { return duration > 0.0; }
    /// How much of it has passed, 0 to 1.
    [[nodiscard]] float passed(double now) const {
        if (duration <= 0.0) return 1.0f;
        return static_cast<float>(std::clamp((now - start) / duration, 0.0, 1.0));
    }
};

inline constexpr double kFlashSeconds = 1.0;

/// A shown frame's update (0x005ec8f0): whether the frame is to hide itself.
/// As its time is up it flashes, for a second, and goes; reversed it stays as
/// it is, covered.
inline bool advance(Clock& clock, bool reverse, double now) {
    if (!clock.runs() || now - clock.start < clock.duration) return false;
    if (clock.flash) {
        clock = {};
        return true;
    }
    if (!reverse) clock = {.start = now, .duration = kFlashSeconds, .flash = true};
    return false;
}

struct Point {
    float x = 0.0f, y = 0.0f;   ///< of the frame, from its top left corner
    friend bool operator==(const Point&, const Point&) = default;
};

/// The eight points the hand passes, from twelve o'clock clockwise, and that
/// again: an eighth of the time to each half of a side (0x005eb130 - 0x005eb9a0).
inline constexpr std::array<Point, 9> kPath = {{{0.5f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f},
                                                {1.0f, 1.0f}, {0.5f, 1.0f}, {0.0f, 1.0f},
                                                {0.0f, 0.5f}, {0.0f, 0.0f}, {0.5f, 0.0f}}};

/// Where the hand is once that much has passed.
[[nodiscard]] inline Point hand(float passed) {
    const float at = std::clamp(passed, 0.0f, 1.0f) * 8.0f;
    const int i = std::min(7, static_cast<int>(at));
    const float f = at - static_cast<float>(i);
    const Point &a = kPath[static_cast<size_t>(i)], &b = kPath[static_cast<size_t>(i + 1)];
    return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}

/// The rim of what is covered, which with the middle makes it: what is left,
/// from the hand on to twelve o'clock, so that it is uncovered clockwise - or,
/// reversed, what has passed.
[[nodiscard]] inline std::vector<Point> covered(float passed, bool reverse) {
    const float at = std::clamp(passed, 0.0f, 1.0f) * 8.0f;
    const int i = std::min(7, static_cast<int>(at));
    std::vector<Point> rim;
    if (reverse) {
        for (int k = 0; k <= i; ++k) rim.push_back(kPath[static_cast<size_t>(k)]);
        rim.push_back(hand(passed));
    } else {
        rim.push_back(hand(passed));
        for (int k = i + 1; k <= 8; ++k) rim.push_back(kPath[static_cast<size_t>(k)]);
    }
    return rim;
}

/// The alpha of the black over the icon, of 255 (0x005ec790).
inline constexpr float kShade = 160.0f / 255.0f;

/// The flash at a moment of it (0x005ec1e0): how its picture is turned, how
/// much of it is stretched over the frame, and its alpha, in six steps.
struct Flash {
    float turn = 0.0f;
    float extent = 1.0f;
    float alpha = 0.0f;
};
[[nodiscard]] inline Flash flashAt(float passed) {
    constexpr std::array<float, 7> kTurn = {0.0f, -0.1303f, -0.2592f, -0.3927f, -0.5184f, -0.6518f, -0.7854f};
    constexpr std::array<float, 7> kExtent = {1.0f, 0.6993f, 0.5405f, 0.6289f, 0.7519f, 0.625f, 0.8696f};
    constexpr std::array<float, 7> kAlpha = {0.0f, 0.5f, 1.0f, 0.75f, 0.5f, 0.25f, 0.0f};
    const float at = std::clamp(passed, 0.0f, 1.0f) * 6.0f;
    const size_t i = static_cast<size_t>(std::min(5, static_cast<int>(at)));
    const float f = at - static_cast<float>(i);
    const auto between = [&](const std::array<float, 7>& t) { return t[i] + (t[i + 1] - t[i]) * f; };
    return {.turn = between(kTurn), .extent = between(kExtent), .alpha = between(kAlpha)};
}

/// A picture's four corners as they are sampled, turned and stretched about
/// its middle: top left, top right, bottom left, bottom right.
[[nodiscard]] inline std::array<Point, 4> turnedCorners(float turn, float extent) {
    const float c = std::cos(turn), s = std::sin(turn);
    std::array<Point, 4> out;
    constexpr std::array<Point, 4> kCorners = {{{-0.5f, -0.5f}, {0.5f, -0.5f}, {-0.5f, 0.5f}, {0.5f, 0.5f}}};
    for (size_t i = 0; i < 4; ++i) {
        const Point& p = kCorners[i];
        out[i] = {(p.x * c - p.y * s) * extent + 0.5f, (p.x * s + p.y * c) * extent + 0.5f};
    }
    return out;
}

}  // namespace wowee::ui::cooldown
