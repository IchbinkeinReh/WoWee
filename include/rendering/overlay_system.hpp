#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace wowee {
namespace rendering {

class VkContext;

/// Manages the fullscreen overlay Vulkan pipelines. (The circle under the
/// target is BlobShadowRenderer's, laid on the ground as the client does.)
/// Extracted from Renderer to isolate overlay rendering resources.
class OverlaySystem {
public:
    explicit OverlaySystem(VkContext* ctx);
    ~OverlaySystem();

    OverlaySystem(const OverlaySystem&) = delete;
    OverlaySystem& operator=(const OverlaySystem&) = delete;

    // Fullscreen color overlay (underwater tint, etc.)
    void renderOverlay(const glm::vec4& color, VkCommandBuffer cmd);

    /// Underwater tint bounded by a waterline that sweeps across the view as the
    /// camera crosses the surface. lineNdc runs from +1 (line off the bottom, so
    /// nothing is tinted) to -1 (off the top, so everything is), softness spans
    /// the meniscus and rippleAmp bends the edge.
    void renderWaterline(const glm::vec4& color, const glm::mat4& invViewProj,
                         float waterZ, float softness, float rippleAmp,
                         float time, bool hasSeam, VkCommandBuffer cmd);

    // Fullscreen multiplicative brightness scale (scene.rgb *= scale). Uses a
    // dst-color blend so brightness > 1 truly scales luminance instead of
    // lerping toward white (which washed everything out). scale > 1 only.
    void renderBrightnessScale(float scale, VkCommandBuffer cmd);

    /// Destroy all Vulkan resources (called before VkContext teardown).
    void cleanup();

    /// Recreate pipelines after swapchain resize / MSAA change.
    void recreatePipelines();

private:
    void initOverlayPipeline();
    void initBrightnessPipeline();

    VkContext* vkCtx_ = nullptr;

    // Fullscreen overlay resources
    VkPipeline overlayPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout_ = VK_NULL_HANDLE;
    // Multiplicative brightness pipeline (shares overlayPipelineLayout_).
    VkPipeline brightnessPipeline_ = VK_NULL_HANDLE;
};

} // namespace rendering
} // namespace wowee
