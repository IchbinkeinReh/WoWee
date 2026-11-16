#pragma once

/// The client's floating text over units (WorldText.cpp, PlayerName.cpp):
/// each line has a kind (0x007e6030's second argument) and the kind's entry
/// in the table at 0x00af4748 decides how it rises, fades and is sized.
/// Arithmetic only: the caller projects and draws.

#include <array>
#include <cmath>
#include <cstdint>

namespace wowee::game::world_text {

/// One kind's entry (0x1c bytes at 0x00af4748 + kind * 0x1c).
struct Kind {
    float rise = 0.0f;            ///< +0x00: yards risen over the whole life
    uint32_t fadeInMs = 0;        ///< +0x04
    uint32_t fadeOutStartMs = 0;  ///< +0x08
    uint32_t lifeMs = 0;          ///< +0x0c
    float scaleStart = 0.0f;      ///< +0x10: text height, interface units
    float scaleEnd = 0.0f;        ///< +0x14
};

/// Kinds 0..10: 0 damage (and a mechanic), 1 absorb, 2 crit, 3 a miss and
/// its kin, 4 experience, 5 an honourable kill, 6 a heal, 7 a crit heal,
/// 8 and 9 the large rising line, 10 threat.
inline constexpr float kSmall = 0.018333333f;
inline constexpr float kLarge = 0.0275f;
inline constexpr std::array<Kind, 11> kKinds{{
    {2.0f, 150, 760, 1500, kSmall, kSmall},
    {2.0f, 150, 90, 1500, kSmall, kSmall},
    {0.0f, 150, 1000, 1500, 0.0f, kLarge},
    {2.0f, 150, 1000, 1500, kSmall, kSmall},
    {0.0f, 500, 2000, 4500, kSmall, kSmall},
    {0.0f, 500, 2000, 4500, kSmall, kSmall},
    {2.0f, 150, 760, 1500, kSmall, kSmall},
    {0.0f, 150, 1000, 1500, 0.0f, kLarge},
    {6.0f, 150, 760, 1500, kLarge, kLarge},
    {6.0f, 150, 760, 1500, kLarge, kLarge},
    {2.0f, 150, 760, 1500, kSmall, kSmall},
}};

[[nodiscard]] constexpr const Kind& kindOf(uint8_t kind) {
    return kKinds[kind < kKinds.size() ? kind : 0];
}

/// A unit holds at most this many lines at once; another is dropped
/// (0x007e5c30's four slots).
inline constexpr int kMaxPerUnit = 4;
/// The line stands this far under the unit's name position (0x007e5c30).
inline constexpr float kBelowName = 1.0f / 3.0f;
/// No line over the camera's unit while the camera is this close: first
/// person (0x007e6030 -> 0x005fe3a0).
inline constexpr float kFirstPersonDistance = 1.8315002f;

[[nodiscard]] constexpr bool expired(uint8_t kind, uint32_t ms) { return ms >= kindOf(kind).lifeMs; }

/// The text's alpha (0..255) and its shadow's (0..127) at `ms` (0x007e6850):
/// up from nothing over the fade-in, held, then down to nothing from the
/// fade-out start to the end of the life.
struct Alpha {
    uint8_t text = 255;
    uint8_t shadow = 127;
};
[[nodiscard]] inline Alpha alphaAt(uint8_t kind, uint32_t ms) {
    const Kind& k = kindOf(kind);
    Alpha a;
    if (ms < k.fadeInMs) {
        const float f = static_cast<float>(ms) / static_cast<float>(k.fadeInMs);
        const long t = std::lround(f * 255.0f);
        a.text = t < 0x100 ? static_cast<uint8_t>(t) : 255;
        const long s = std::lround(f * 127.0f);
        a.shadow = static_cast<uint8_t>(s > 127 ? 127 : s);
    } else if (ms >= k.fadeOutStartMs) {
        const uint32_t span = k.lifeMs - k.fadeOutStartMs;
        const float f = span == 0 ? 0.0f : static_cast<float>(ms - k.fadeOutStartMs) / static_cast<float>(span);
        const float t = std::fmax(0.0f, std::fmin(255.0f, f * 255.0f));
        a.text = static_cast<uint8_t>(std::lround(255.0f - t));
        // 127 less it for the shadow (the x87 code at 0x007e69a0 subtracts
        // from the 127 it loaded; the decompiler shows 255 there).
        const float s = std::fmax(0.0f, std::fmin(127.0f, f * 127.0f));
        a.shadow = static_cast<uint8_t>(std::lround(127.0f - s));
    }
    return a;
}

/// The crit lines' pop (0x007e6cc0, segments at 0x00a41618): 0.1 to 2 over
/// the first tenth of the life, 2 back to 1 by the fifth, then 1.
[[nodiscard]] constexpr float critPop(float t) {
    struct Segment { float t0, t1, s0, s1; };
    constexpr Segment kPop[3] = {{0.0f, 0.1f, 0.1f, 2.0f}, {0.1f, 0.2f, 2.0f, 1.0f}, {0.2f, 1.0f, 1.0f, 1.0f}};
    for (const Segment& s : kPop)
        if (t >= s.t0 && t < s.t1) return (s.s1 - s.s0) * ((t - s.t0) / (s.t1 - s.t0)) + s.s0;
    return 1.0f;
}

/// The text's height in interface units at `ms` (0x007e6cc0): the crit
/// kinds (2 and 7) the pop times their end size, the others from their start
/// size to their end one; never under 0.001.
[[nodiscard]] constexpr float scaleAt(uint8_t kind, uint32_t ms) {
    const Kind& k = kindOf(kind);
    const float t = static_cast<float>(ms) / static_cast<float>(k.lifeMs);
    const float s = (kind == 2 || kind == 7) ? critPop(t) * k.scaleEnd : (k.scaleEnd - k.scaleStart) * t + k.scaleStart;
    return s <= 0.001f ? 0.001f : s;
}

/// Yards risen at `ms` (0x007e6590).
[[nodiscard]] constexpr float riseAt(uint8_t kind, uint32_t ms) {
    const Kind& k = kindOf(kind);
    return static_cast<float>(ms) / static_cast<float>(k.lifeMs) * k.rise;
}

/// Interface units to pixels: the client's screen is a diagonal of one
/// (0x0047bf90 sets the height to 1/sqrt(aspect^2 + 1)), so a height of `s`
/// is s * sqrt(width^2 + height^2) pixels (0x007e66f0 -> 0x0047c060).
[[nodiscard]] inline float pixels(float s, float screenW, float screenH) {
    return s * std::sqrt(screenW * screenW + screenH * screenH);
}

/// The text's centre kept on the screen (0x007e70d0): no nearer an edge
/// than half the text.
[[nodiscard]] constexpr float clampCentre(float at, float half, float extent) {
    float c = at < half ? half : at;
    if (extent - half <= c) return extent - half;
    return c;
}

/// The rank badge an honourable kill's line carries (0x007e7390): ranks 5
/// to 19 are PvPRank01..15, drawn 40 pixels left of the text, as tall as
/// it. 0 for none.
[[nodiscard]] constexpr uint32_t rankBadge(uint32_t rank) {
    return rank > 4 && rank - 5 < 15 ? rank - 4 : 0;
}

}  // namespace wowee::game::world_text
