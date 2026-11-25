#pragma once

/// The ways one image of the interface is uploaded, each its own.

#include "ui/widget_tree.hpp"

#include <cstdint>
#include <string>

namespace wowee::ui {

/// Texture:SetDesaturated: the art's luminance (0.299, 0.587, 0.114) in all
/// three channels, as the interface's Desaturate.bls draws it (CSimpleTop,
/// 0x00483060).
inline constexpr uint8_t kVariantDesaturate = 2;
/// A tabard designer's emblem: white by the art's alpha.
inline constexpr uint8_t kVariantAlphaMask = 4;
/// Repeated past its edges, across and down (0x00681be0: bits 3 and 4 of the
/// flags, the sampler's two directions, 0x00a2f9c4).
inline constexpr uint8_t kVariantRepeatAcross = 8;
inline constexpr uint8_t kVariantRepeatDown = 16;

/// A texture region's.
[[nodiscard]] inline uint8_t textureVariantOf(const Widget& w) {
    return static_cast<uint8_t>((w.desaturated ? kVariantDesaturate : 0) | (w.emblemMask ? kVariantAlphaMask : 0) |
                                (w.repeatAcross ? kVariantRepeatAcross : 0) |
                                (w.repeatDown ? kVariantRepeatDown : 0));
}

/// A backdrop's background: a tiled one is repeated across alone (0x004a2e00).
[[nodiscard]] inline uint8_t backdropVariantOf(const Widget& w) {
    return w.tileBackground ? kVariantRepeatAcross : 0;
}

/// How often a tiled background stands in a side of `size` units (0x004a29d0):
/// by its tile size, by the edge's where it has none.
[[nodiscard]] inline float backdropRepeats(const Widget& w, float size) {
    const float tile = w.backdropTileSize > 0.0f ? w.backdropTileSize : w.edgeSize;
    return w.tileBackground && tile > 0.0f ? size / tile : 1.0f;
}

/// What an upload is known by.
[[nodiscard]] inline std::string textureKey(const std::string& path, uint8_t variant) {
    if (variant == 0) return path;
    std::string key = path;
    if (variant & kVariantDesaturate) key += "|desat";
    if (variant & kVariantAlphaMask) key += "|mask";
    if (variant & kVariantRepeatAcross) key += "|across";
    if (variant & kVariantRepeatDown) key += "|down";
    return key;
}

}  // namespace wowee::ui
