#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/blob_shadow.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class TerrainManager;
class WMORenderer;
class M2Renderer;

/// The client's blob shadows (0x007e49e0 and below): for each unit, the
/// ground's triangles under it - terrain, buildings, doodads - gathered on the
/// CPU and drawn again with Textures\ShadowBlob.blp projected down onto them
/// and the ground multiplied by the result.
class BlobShadowRenderer {
public:
    BlobShadowRenderer() = default;
    ~BlobShadowRenderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                  pipeline::AssetManager* assets);
    void shutdown();
    void recreatePipelines();

    /// Gather this frame's ground under every caster in view. On the main
    /// thread, before the frame's command buffers are recorded.
    void prepare(uint32_t frameIndex, const std::vector<blob_shadow::Caster>& casters, const glm::mat4& viewProj,
                 const TerrainManager* terrain, const WMORenderer* wmo, const M2Renderer* m2);
    /// Draw what prepare gathered.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

private:
    struct Push {
        glm::vec4 uRow;
        glm::vec4 vRow;
        glm::vec4 hRow;
        glm::vec4 params;
    };
    struct Draw {
        Push push;
        uint32_t firstVertex = 0;
        uint32_t vertexCount = 0;
    };

    bool createPipeline();

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet textureSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkTexture blobTexture_;

    static constexpr uint32_t kMaxVertices = 0xC000 * 4;
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> vertexBuffer_{};
    std::array<VmaAllocation, MAX_FRAMES_IN_FLIGHT> vertexAlloc_{};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> vertexMapped_{};
    uint32_t frame_ = 0;
    std::vector<Draw> draws_;
    std::vector<glm::vec3> scratch_;
};

}  // namespace rendering
}  // namespace wowee
