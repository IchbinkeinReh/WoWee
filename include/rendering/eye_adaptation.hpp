#pragma once

/// The eye's adaptation to the light under the ray traced lighting: how
/// bright the picture is, measured; how far the eye has followed that; and
/// what exposure that gives (EyeAdaptationPass applies it).
///
/// The original has none - its picture is lit for one eye, the artists'.
/// The traced lighting changes how much light reaches a room or a street
/// from what they were tuned under, and stepping from the sun into a
/// lamplit inn, or back out, the eye should take a moment to follow.
///
/// It is held to a narrow band. Inside the band of what daylight outdoors
/// measures the exposure is exactly one, so a day in the open looks as it
/// does without it; below it a dark room is brought up a little, above it
/// a glaring one taken down a little - each only part of the way, and
/// never by more than kMaxExposure or kMinExposure.
///
/// Pure: what tests/test_eye_adaptation.cpp checks.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::rendering::eye_adaptation {

struct Params {
    /// log2 of the mean luminance (linear, 0..1, of the middle of the
    /// screen) the neutral band spans: what the world measures by day -
    /// -4 on the Barrens' open ground at noon, -5 to -5.8 under Elwynn's
    /// trees and in Stormwind's and Westfall's streets, -5.1 in Goldshire's
    /// lamplit inn - with some room either side.
    float neutralLow = -6.3f;
    float neutralHigh = -3.6f;
    /// The share of the distance outside the band that the exposure makes
    /// up: a room four times darker than the band is lit twice as bright
    /// at one half, not four times.
    float strength = 0.5f;
    float minExposure = 0.75f;
    float maxExposure = 1.3f;
    /// How fast the eye follows, in seconds to come 63% of the way: into
    /// brighter light fast (the pupil closes in well under a second), into
    /// the dark slowly (it opens over seconds).
    float toBrighterSeconds = 0.5f;
    float toDarkerSeconds = 2.5f;
};

/// The two settings (Ray Tracing, "Eye adaptation"): how far the eye may
/// brighten a dark view, in percent over one, and how long it takes to
/// settle into the dark, in seconds. Their defaults are the Params above.
constexpr int kDefaultBrighteningPercent = 30;
constexpr int kMaxBrighteningPercent = 100;
constexpr float kDefaultSeconds = 2.5f;
constexpr float kMinSeconds = 0.5f;
constexpr float kMaxSeconds = 20.0f;

/// The Params those two settings give.
///
/// The brightening is the most a dark view is lifted: 30 is x1.3, 100 x2.
/// The most a glaring one is taken down goes with it, by the same share of
/// stops - x0.75 at 30, never below x0.5 - so 0 is no adjustment at all,
/// an exposure of exactly one, the picture as the original has it.
///
/// The time is toward the dark; toward the light the eye stays five times
/// quicker, as it is by default (2.5 s and 0.5 s).
inline Params paramsFor(int brighteningPercent, float seconds) {
    Params p;
    const int percent = std::clamp(brighteningPercent, 0, kMaxBrighteningPercent);
    if (percent != kDefaultBrighteningPercent) {
        const float lift = static_cast<float>(percent) / 100.0f;
        const float share = std::log2(1.0f + lift) / std::log2(1.0f + kDefaultBrighteningPercent / 100.0f);
        p.maxExposure = 1.0f + lift;
        p.minExposure = std::max(0.5f, std::pow(0.75f, share));
    }
    const float darker = std::isfinite(seconds) ? std::clamp(seconds, kMinSeconds, kMaxSeconds) : kDefaultSeconds;
    if (darker != kDefaultSeconds) {
        p.toDarkerSeconds = darker;
        p.toBrighterSeconds = darker / 5.0f;
    }
    return p;
}

/// The exposure an eye adapted to `adaptedLog2` sees with.
inline float exposureFor(float adaptedLog2, const Params& p = {}) {
    float stops = 0.0f;
    if (adaptedLog2 < p.neutralLow) stops = (p.neutralLow - adaptedLog2) * p.strength;
    else if (adaptedLog2 > p.neutralHigh) stops = (p.neutralHigh - adaptedLog2) * p.strength;
    return std::clamp(std::exp2(stops), p.minExposure, p.maxExposure);
}

/// The adaptation `dt` seconds on, toward what was just measured: an
/// exponential approach, quicker toward brighter light than toward darker.
inline float adapt(float adaptedLog2, float measuredLog2, float dt, const Params& p = {}) {
    if (!(dt > 0.0f)) return adaptedLog2;
    const float tau = measuredLog2 > adaptedLog2 ? p.toBrighterSeconds : p.toDarkerSeconds;
    const float k = 1.0f - std::exp(-dt / std::max(tau, 1e-3f));
    return adaptedLog2 + (measuredLog2 - adaptedLog2) * k;
}

/// Where an eye that has just started looking begins: what it measures,
/// held into the neutral band - so it starts at an exposure of one and
/// adapts from there, rather than starting adapted.
inline float initialAdaptation(float measuredLog2, const Params& p = {}) {
    return std::clamp(measuredLog2, p.neutralLow, p.neutralHigh);
}

/// log2 of the mean luminance of an 8-bit picture, `width` x `height`
/// pixels four bytes each, in display encoding: blue first when `bgra`.
/// The mean is of the logs (a geometric mean), so one lamp's glare does not
/// outweigh the room around it.
inline float measureLog2(const uint8_t* pixels, uint32_t width, uint32_t height, bool bgra) {
    if (!pixels || width == 0 || height == 0) return 0.0f;
    // Display encoding to linear, by a power of 2.2, per byte.
    static const auto kLinear = [] {
        struct Table { float v[256]; } t{};
        for (int i = 0; i < 256; ++i) t.v[i] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
        return t;
    }();
    const int r = bgra ? 2 : 0, b = bgra ? 0 : 2;
    double sum = 0.0;
    const size_t count = size_t(width) * height;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* px = pixels + i * 4;
        const float lum = 0.2126f * kLinear.v[px[r]] + 0.7152f * kLinear.v[px[1]] + 0.0722f * kLinear.v[px[b]];
        // A floor, so a black pixel counts as very dark and not as minus
        // infinity.
        sum += std::log2(std::max(lum, 1.0f / 1024.0f));
    }
    return static_cast<float>(sum / static_cast<double>(count));
}

}  // namespace wowee::rendering::eye_adaptation
