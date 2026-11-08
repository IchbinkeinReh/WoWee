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
#include <functional>
#include <vector>

struct ImTextureData;

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

    /// The text there is now, as triangles, and the font texture their
    /// glyphs were laid out on. Replaces what was there.
    ///
    /// The texture, not the atlas: the interface's atlas is repacked into a
    /// new texture when it fills, every glyph moved, and the triangles hold
    /// the old packing's coordinates. Drawn from the atlas's texture of the
    /// moment, they read garbage for the frame. The texture's descriptor is
    /// read when the text is drawn - one made this frame has none until the
    /// interface has drawn.
    /// One unit's name among the triangles: whose it is, where the unit was
    /// drawn when it was laid out, and which vertices are its.
    struct Run {
        uint64_t guid = 0;
        glm::vec3 base{0.0f};
        size_t first = 0;
        size_t count = 0;
    };
    void setText(std::vector<Vertex> triangles, ImTextureData* texture, std::vector<Run> runs = {});
    void clear() {
        triangles_.clear();
        runs_.clear();
        texture_ = nullptr;
    }

    /// Each name moved with its unit to where the unit is drawn this frame.
    ///
    /// The names are laid out by the interface after the world is drawn, so
    /// the scene draws the last frame's: on a zeppelin under way, thirty
    /// yards a second, every name over its deck hung a frame's travel behind
    /// the unit it belongs to. `drawnAt` gives a unit's position now (false
    /// for one not drawn); a name's anchors move by how far it has gone since
    /// the layout. Called before the world is drawn.
    void followUnits(const std::function<bool(uint64_t, glm::vec3&)>& drawnAt);

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
    std::vector<Run> runs_;
    ImTextureData* texture_ = nullptr;
    int textFrame_ = -1;  ///< the interface frame the text was laid out in
};

}  // namespace wowee::rendering
