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
#include <vector>

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
///
/// The sensitivity narrows the neutral band about its middle: 0 is the band
/// as it is (2.7 stops, -6.3 to -3.6), 50 half as wide, 100 none at all -
/// every view a little off the middle is brightened or darkened toward it.
/// The higher, the smaller a difference from daylight already moves the
/// eye.
constexpr int kDefaultSensitivityPercent = 0;
constexpr int kMaxSensitivityPercent = 100;

inline Params paramsFor(int brighteningPercent, float seconds, int sensitivityPercent = kDefaultSensitivityPercent) {
    Params p;
    const int sensitivity = std::clamp(sensitivityPercent, 0, kMaxSensitivityPercent);
    if (sensitivity != kDefaultSensitivityPercent) {
        const float middle = 0.5f * (p.neutralLow + p.neutralHigh);
        const float half = 0.5f * (p.neutralHigh - p.neutralLow) * (1.0f - static_cast<float>(sensitivity) / 100.0f);
        p.neutralLow = middle - half;
        p.neutralHigh = middle + half;
    }
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

/// The exposure over a finished picture whose highlights are already clipped.
///
/// The frame is drawn straight into the 8-bit swapchain, so the sky round a
/// low sun - far brighter than white - is stored as white, 1.0, and a plain
/// multiply by an exposure under one made it a flat grey disc where the
/// picture without the traced lighting is white. So where the frame is
/// clipped to white in all three channels, and round it, the exposure is let
/// go toward one: the burnt-out sky stays white, as it is without the traced
/// lighting, and the glare falls off softly into the exposed sky round it.
///
/// Per cell of the frame (eye_highlight_mask.comp.glsl) the share of pixels
/// clipped white (clipShare); that blurred wide - a 5 x 5 Gaussian over the
/// cells, kGlareSpread cells apart - gives the glare weight (glareWeight,
/// with a tighter blur for a small core on its own),
/// and the pixel's colour is scaled by the exposure mixed toward one by it
/// (glareScale; eye_adaptation.frag.glsl). Only ever a scale, so a pale
/// coloured cloud keeps its hue; only white in all three channels counts, so
/// a sunlit yellow field or a blue noon sky is never mistaken for it; and far
/// from any clipped white - by day, in an inn - it is exactly the multiply.
/// Above an exposure of one (a dark room brightened) it is the multiply too.
constexpr float kClipLow = 0.96f;
constexpr float kClipHigh = 0.995f;
constexpr float kGlareGain = 2.5f;
constexpr float kGlareSpread = 2.0f;
/// A small clipped core on its own - the sun's disc low over a ridge - is
/// too small to count in the wide blur: a tighter one (kCoreSpread cells
/// apart) brings it up too, but only where the wide one sees next to nothing
/// (below kIsolatedLow..kIsolatedHigh), so a large burnt-out sky keeps the
/// wide blur's soft edge.
constexpr float kCoreGain = 3.0f;
constexpr float kCoreSpread = 0.5f;
constexpr float kIsolatedLow = 0.02f;
constexpr float kIsolatedHigh = 0.15f;

/// The cell, in pixels a side, for a frame `height` pixels high: 32 at 1080,
/// so the glare's reach is the same share of the screen at any size.
inline uint32_t glareCell(uint32_t height) {
    return std::max(4u, static_cast<uint32_t>(std::lround(static_cast<double>(height) / 33.75)));
}

inline float smoothstep01(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// How far one pixel (0..1 each channel) is clipped white: 0 unless all
/// three channels are at the top.
inline float clipShare(float r, float g, float b) {
    return smoothstep01(kClipLow, kClipHigh, std::min(r, std::min(g, b)));
}

/// The glare weight from the share of clipped white round a pixel, blurred
/// wide (kGlareSpread) and tight (kCoreSpread).
inline float glareWeight(float wideShare, float coreShare) {
    const float wide = smoothstep01(0.0f, 1.0f, std::clamp(wideShare * kGlareGain, 0.0f, 1.0f));
    const float core = smoothstep01(0.0f, 1.0f, std::clamp(coreShare * kCoreGain, 0.0f, 1.0f));
    return std::max(wide, core * (1.0f - smoothstep01(kIsolatedLow, kIsolatedHigh, wideShare)));
}

/// What a pixel's colour is multiplied by.
inline float glareScale(float exposure, float weight) {
    if (!(exposure < 1.0f)) return exposure;
    return exposure + (1.0f - exposure) * std::clamp(weight, 0.0f, 1.0f);
}

/// The whole of it on the CPU, for the tests: `rgb` is `width` x `height`
/// pixels, three floats each, 0..1, exposed in place. The mask texels and
/// the composite's filtered taps as the shaders have them.
inline void exposeFrame(float* rgb, uint32_t width, uint32_t height, float exposure) {
    if (!rgb || width == 0 || height == 0) return;
    const uint32_t cell = glareCell(height);
    const uint32_t mw = (width + cell - 1) / cell, mh = (height + cell - 1) / cell;
    std::vector<float> mask(size_t(mw) * mh, 0.0f);
    for (uint32_t my = 0; my < mh; ++my) {
        for (uint32_t mx = 0; mx < mw; ++mx) {
            float sum = 0.0f, count = 0.0f;
            for (uint32_t y = my * cell; y < std::min(height, (my + 1) * cell); ++y) {
                for (uint32_t x = mx * cell; x < std::min(width, (mx + 1) * cell); ++x) {
                    const float* p = rgb + (size_t(y) * width + x) * 3;
                    sum += clipShare(p[0], p[1], p[2]);
                    count += 1.0f;
                }
            }
            mask[size_t(my) * mw + mx] = count > 0.0f ? sum / count : 0.0f;
        }
    }
    // A linear, clamped-to-edge fetch of the mask at mask texel coordinates.
    const auto fetch = [&](float u, float v) {
        u = std::clamp(u - 0.5f, 0.0f, float(mw - 1));
        v = std::clamp(v - 0.5f, 0.0f, float(mh - 1));
        const uint32_t x0 = uint32_t(u), y0 = uint32_t(v);
        const uint32_t x1 = std::min(x0 + 1, mw - 1), y1 = std::min(y0 + 1, mh - 1);
        const float fx = u - float(x0), fy = v - float(y0);
        const auto m = [&](uint32_t x, uint32_t y) { return mask[size_t(y) * mw + x]; };
        return (m(x0, y0) * (1 - fx) + m(x1, y0) * fx) * (1 - fy) + (m(x0, y1) * (1 - fx) + m(x1, y1) * fx) * fy;
    };
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float scale = exposure;
            if (exposure < 1.0f) {
                const float u = (float(x) + 0.5f) / float(cell), v = (float(y) + 0.5f) / float(cell);
                const auto share = [&](float spread) {
                    float sum = 0.0f, weights = 0.0f;
                    for (int dy = -2; dy <= 2; ++dy) {
                        for (int dx = -2; dx <= 2; ++dx) {
                            const float w = std::exp(-float(dx * dx + dy * dy) / 4.5f);
                            sum += w * fetch(u + dx * spread, v + dy * spread);
                            weights += w;
                        }
                    }
                    return sum / weights;
                };
                scale = glareScale(exposure, glareWeight(share(kGlareSpread), share(kCoreSpread)));
            }
            float* p = rgb + (size_t(y) * width + x) * 3;
            for (int c = 0; c < 3; ++c) p[c] = std::clamp(p[c] * scale, 0.0f, 1.0f);
        }
    }
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
