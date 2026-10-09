#pragma once

// How the interface's art is prepared before it is uploaded, for the ways a
// Texture can ask to be drawn that a single ImGui draw list cannot: additive
// blending, and Texture:SetDesaturated.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee {
namespace ui {

/// Additive art over a dark scene: alpha taken from brightness, so black adds
/// nothing. RGBA8.
inline void prepareAdditive(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const uint8_t lum = std::max({rgba[i], rgba[i + 1], rgba[i + 2]});
        rgba[i + 3] = static_cast<uint8_t>((rgba[i + 3] * lum) / 255);
    }
}

/// Desaturate.bls (the interface's, CSimpleTop 0x00483060): each texel's
/// luminance, dot(rgb, (0.299, 0.587, 0.114)), in all three channels, its
/// alpha kept. RGBA8.
inline void prepareDesaturated(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const float l = 0.299f * rgba[i] + 0.587f * rgba[i + 1] + 0.114f * rgba[i + 2];
        const uint8_t b = static_cast<uint8_t>(std::min(255.0f, l + 0.5f));
        rgba[i] = rgba[i + 1] = rgba[i + 2] = b;
    }
}

}  // namespace ui
}  // namespace wowee
