#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>

#include "rendering/scene_depth_view.hpp"
#include "rendering/screen_target.hpp"

namespace wowee {
namespace rendering {

class VkContext;

/**
 * Crepuscular rays drawn over the finished picture: the bright sky around the
 * sun smeared outward from the sun's place on screen, so every gap in a canopy
 * or between two roofs pours a streak of light toward the camera.
 *
 * It complements the volumetric fog rather than repeating it. The fog's shafts
 * are real light in real air and show only where there is air enough to see
 * them; these are what the eye does looking into the sun, and show whenever it
 * is on screen or just off it.
 *
 * Only the sky streams. The rays are gathered from the finished frame, but
 * only where the scene's depth holds nothing in front of the far plane - the
 * sky, its clouds, the sun's disc - so the terrain, the buildings and the WDL
 * horizon block the rays and never send any of their own, however brightly
 * they are lit. Gathered from brightness alone, a sunlit slope or wall near
 * the sun on screen poured its own rays over everything in front of it and
 * washed the picture out. The depth is the scene's own, wherever this frame's
 * path left it (FrameInputs), read in the same frame: nothing here is carried
 * over from an earlier one.
 *
 * Each frame: the swapchain image is blitted to a quarter-size copy, a compute
 * pass keeps of it only the sky (sun_shafts_mask.comp.glsl), another marches
 * every pixel toward the sun gathering the sky it passes, and the result is
 * added into the overlay pass ahead of the UI.
 * Double-buffered per frame in flight.
 */
class SunShafts {
public:
    /// What one frame's rays come from.
    struct FrameInputs {
        /// The sun's place on screen, framebuffer uv. May be off the edge.
        glm::vec2 sunUV{0.5f};
        /// The sun's colour, which the rays take on.
        glm::vec3 tint{1.0f};
        /// 0 draws nothing this frame.
        float strength = 0.0f;
        /// The scene's depth, as the scene's last pass left it: single- or
        /// multisampled, at the scene's size (an upscaler's is smaller), in
        /// depthLayout. Without one there is no telling sky from ground, and
        /// nothing is drawn.
        VkImage depthImage = VK_NULL_HANDLE;
        bool depthMultisampled = false;
        VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        /// Changes whenever the scene's targets are made again: a new depth
        /// image can come back with the old one's handle.
        uint64_t depthGeneration = 0;
    };

    SunShafts() = default;
    ~SunShafts();
    SunShafts(const SunShafts&) = delete;
    SunShafts& operator=(const SunShafts&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx);
    void shutdown();

    /// Build this frame's rays from the finished swapchain image. Outside any
    /// render pass, after the scene's last one has closed - the image is in
    /// PRESENT_SRC and goes back there. Returns whether there is anything for
    /// composite() to add.
    bool record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                VkExtent2D extent, const FrameInputs& in);

    /// Add them to the picture. Inside the overlay pass, viewport set, before
    /// the interface is drawn. Draws nothing unless record() said there was
    /// something this frame.
    void composite(VkCommandBuffer cmd, uint32_t frame);

private:
    static constexpr uint32_t MAX_FRAMES = 2;

    using Target = ScreenTarget;
    bool ensureTargets(VkExtent2D extent);
    void destroyTargets();
    bool ensureCompositePipeline();

    VkContext* ctx_ = nullptr;
    bool usable_ = false;

    // Quarter the swapchain's size, per frame slot: the copy of the frame, its
    // sky (the rest black), and the rays marched out of that.
    Target frameCopy_[MAX_FRAMES];
    Target source_[MAX_FRAMES];
    Target rays_[MAX_FRAMES];
    VkExtent2D targetExtent_{.width = 0, .height = 0};
    VkExtent2D sourceExtent_{.width = 0, .height = 0};
    bool drawThisFrame_[MAX_FRAMES] = {};

    VkSampler sampler_ = VK_NULL_HANDLE;  // owned by the context cache
    VkSampler pointSampler_ = VK_NULL_HANDLE;  // the depth's; also the cache's

    // The depth view, and which one each slot's mask set was last written
    // with (by serial: a destroyed view's handle can be given out again).
    SceneDepthView depthView_;
    uint64_t maskSetSerial_[MAX_FRAMES] = {};

    VkDescriptorSetLayout maskSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout maskLayout_ = VK_NULL_HANDLE;
    VkPipeline maskPipeline_ = VK_NULL_HANDLE;    // single-sampled depth
    VkPipeline maskPipelineMs_ = VK_NULL_HANDLE;  // multisampled, unresolved

    VkDescriptorSetLayout marchSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout marchLayout_ = VK_NULL_HANDLE;
    VkPipeline marchPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    /// The swapchain format the composite pipeline was built for. The overlay
    /// pass is rebuilt with the swapchain, and a pipeline stays valid for any
    /// pass with the same attachment - so only a format change rebuilds it.
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet maskSets_[MAX_FRAMES] = {};
    VkDescriptorSet marchSets_[MAX_FRAMES] = {};
    VkDescriptorSet compositeSets_[MAX_FRAMES] = {};
};

} // namespace rendering
} // namespace wowee
