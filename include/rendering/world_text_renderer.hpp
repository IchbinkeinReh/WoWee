#pragma once

/// Text standing in the world: the names over heads (PlayerName.cpp,
/// 0x007e5640).
///
/// The client writes a unit's name into a CGxStringBatch made with its world
/// flag (0x007e64d0 -> 0x006bf160(1, 1)), and a world batch is drawn with the
/// depth test and the depth write on and an alpha reference of 1
/// (0x006c53a0, render states 0xd, 0xf and 7). The name is part of the
/// scene: a wall in front of it hides it, pixel by pixel, the same as it
/// hides the unit.
///
/// The glyphs come laid out by the interface's font code, in pixels; each
/// vertex is kept as its anchor in the world and its offset in the plane
/// that faces the camera through that anchor, so the words stand where the
/// unit is when the world is drawn and are tested against its depth.

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

struct ImFontAtlas;

namespace wowee::rendering {

class VkContext;

class WorldTextRenderer {
public:
    /// One corner of a glyph's quad.
    struct Vertex {
        glm::vec3 anchor;  ///< the point the text stands on, render space
        glm::vec2 offset;  ///< yards right and up in the plane facing the camera
        glm::vec2 uv;      ///< in the font texture
        uint32_t colour;   ///< RGBA, 8 bits each (ImU32)
    };

    WorldTextRenderer() = default;
    ~WorldTextRenderer();
    WorldTextRenderer(const WorldTextRenderer&) = delete;
    WorldTextRenderer& operator=(const WorldTextRenderer&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();

    /// The text there is now, as triangles, and the font atlas whose texture
    /// it samples. Replaces what was there. The atlas's texture is looked up
    /// when the text is drawn: the interface replaces it when the atlas grows.
    void setText(std::vector<Vertex> triangles, const ImFontAtlas* atlas);
    void clear() { triangles_.clear(); }

    /// Drawn into the scene's pass, after its geometry, once: the text is
    /// handed over again for every frame it should stand in.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

private:
    void buildPipeline();

    static constexpr uint32_t kFrames = 2;
    static constexpr size_t kMaxVertices = 96 * 1024;

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkBuffer vertexBuffer_[kFrames] = {};
    VmaAllocation vertexAlloc_[kFrames] = {};
    void* vertexMapped_[kFrames] = {};

    std::vector<Vertex> triangles_;
    const ImFontAtlas* atlas_ = nullptr;
};

}  // namespace wowee::rendering
