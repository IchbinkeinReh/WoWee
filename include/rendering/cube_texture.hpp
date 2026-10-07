#pragma once

/// A cube map from a BLP, as the 3.3.5a client's texture loader makes one.
///
/// 0x004b95b0 takes a BLP six times as wide as it is high for a cube map: one
/// face a square, and its upload callback (0x004b7aa0) reads device face f -
/// +X, -X, +Y, -Y, +Z, -Z - from strip square cubeStripSquare(f). Procedural
/// water's two cube units and a terrain chunk's env layer are loaded this way.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>

namespace wowee {
namespace rendering {

class VkContext;

/// Whether 0x004b95b0 loads a w x h BLP as a cube map.
inline bool isCubeStrip(uint32_t width, uint32_t height) { return height > 0 && width == height * 6; }

/// The strip square device face `face` comes from (0x004b7aa0's switch):
/// +X 0, -X 2, +Y 4, -Y 5, +Z 3, -Z 1.
inline int cubeStripSquare(int face) {
    constexpr std::array<int, 6> kSquare{0, 2, 4, 5, 3, 1};
    return face >= 0 && face < 6 ? kSquare[face] : 0;
}

struct CubeTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    [[nodiscard]] bool valid() const { return view != VK_NULL_HANDLE; }
};

/// Uploads RGBA8 pixels as a cube: a six-wide strip face by face as above,
/// anything else (a texture the client would bind to a cube unit as it is)
/// as the one image on every face.
bool uploadCubeTexture(VkContext& ctx, const uint8_t* rgba, uint32_t width, uint32_t height,
                       CubeTexture& out);
void destroyCubeTexture(VkContext& ctx, CubeTexture& cube);

}  // namespace rendering
}  // namespace wowee
