#pragma once

/**
 * normal_map.hpp - a normal map derived from a diffuse texture.
 *
 * The character renderer and the WMO renderer both build one the same way:
 * luminance to height, then a Sobel pass for the normals. Their two copies
 * differed in exactly one value, the Sobel strength, which is now the
 * parameter it always was.
 *
 * Pure on purpose. Both copies lived inside a renderer class and needed a
 * Vulkan context to reach, so neither had ever been tested; this takes pixels
 * and answers pixels.
 */

#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

/// RGBA8 normal map from an RGBA8 diffuse texture.
///
/// The result is width*height*4 bytes: the normal in RGB, alpha 255.
///
/// strength scales the Sobel gradient. The character renderer uses 5, which
/// suits skin and cloth; the WMO renderer uses 2, which suits stone and wood
/// at the size a wall is seen from.
///
/// Answers an empty vector when there is nothing to read.
std::vector<uint8_t> generateNormalMap(const uint8_t* pixels,
                                       uint32_t width, uint32_t height,
                                       float strength);

}  // namespace rendering
}  // namespace wowee
