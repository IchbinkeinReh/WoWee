#pragma once

#include "rendering/vk_texture.hpp"
#include "rendering/water_motes.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <unordered_map>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;
class Camera;

/// Draws what floats in the water about the camera (water_motes.hpp).
class WaterMotesRenderer {
public:
    ~WaterMotesRenderer() { shutdown(); }

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    /// LiquidType.dbc and the texture, once the assets are there.
    void loadAssets(pipeline::AssetManager* assets);
    void recreatePipelines();
    void shutdown();

    /// `liquidType`: what the camera is in, 0 for nothing.
    void update(float deltaTime, const Camera& camera, uint32_t liquidType);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

private:
    bool buildPipeline();

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet textureSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkTexture texture_;
    static constexpr int kFrames = 3;
    VkBuffer buffer_[kFrames] = {};
    VmaAllocation allocation_[kFrames] = {};
    void* mapped_[kFrames] = {};
    int frame_ = 0;
    uint32_t vertices_ = 0;
    bool assetsLoaded_ = false;

    std::unordered_map<uint32_t, water_motes::Liquid> liquids_;
    uint32_t liquidType_ = 0;
    water_motes::Liquid liquid_;
    water_motes::Swarm swarm_;
};

}  // namespace rendering
}  // namespace wowee
