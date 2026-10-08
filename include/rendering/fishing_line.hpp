#pragma once

/// The line from a fishing pole to its bobber (0x007221d0, 0x006f8f50); its
/// shape is in fishing_line_geometry.hpp.

#include "rendering/fishing_line_geometry.hpp"

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace wowee::rendering {

class VkContext;
class Renderer;


/// Draws every fishing line the frame has.
class FishingLineRenderer {
public:
    /// A unit channelling at a bobber (0x007221d0): its CharacterRenderer
    /// instance, whose right hand (attachment 1) holds the pole, and the
    /// bobber's M2Renderer instance.
    struct Line {
        uint32_t unitInstance = 0;
        uint32_t bobberInstance = 0;
    };

    FishingLineRenderer() = default;
    ~FishingLineRenderer();

    [[nodiscard]] bool initialize(Renderer* owner, VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    /// The lines there are now. Each is drawn where its pole and bobber are
    /// when it is drawn; one whose pole has no "$CCH", or whose bobber has
    /// gone, is not.
    void setLines(std::vector<Line> lines) { lines_ = std::move(lines); }
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

private:
    void buildPipeline(VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
                       const VkPipelineShaderStageCreateInfo& fragStage);

    static constexpr size_t kMaxLines = 32;

    Renderer* owner_ = nullptr;
    VkContext* vkCtx_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc_ = VK_NULL_HANDLE;
    VmaAllocationInfo vertexAllocInfo_{};
    std::vector<Line> lines_;
};

}  // namespace wowee::rendering
