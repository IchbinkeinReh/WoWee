#pragma once

// How the interface's art is prepared before it is uploaded, for the ways a
// Texture can ask to be drawn that a single ImGui draw list cannot:
// Texture:SetDesaturated, a tabard's emblem.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee {
namespace ui {

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

/// TabardModel:GetUpperEmblemTexture / GetLowerEmblemTexture (0x00599890):
/// the emblem white, by its alpha alone. RGBA8.
inline void prepareAlphaMask(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) rgba[i] = rgba[i + 1] = rgba[i + 2] = 255;
}

}  // namespace ui
}  // namespace wowee
