#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>

#include "rendering/screen_target.hpp"

namespace wowee {
namespace rendering {

class VkContext;

/**
 * The client's full-screen effects (Wow.exe 3.3.5a), over the finished world
 * and under the interface:
 *
 * - ffxGlow, "full screen glow effect", on by default where shaders are
 *   supported (0x008b0000 registers the cvar; EffectGlow / PassGlow, blurred
 *   by FFXGauss4 and FFXBox4): a blurred copy of the frame added back at the
 *   light's LightParams.Glow strength.
 * - ffxDeath, "full screen death effect" (0x007e86a0, drawn by 0x007e87b0
 *   in the glow's place): the glowed frame's luminance, with the constant
 *   (0x53, 0x93, 0xa8) laid over it by 4 x lum x (1 - lum).
 *
 * Both composites are the FFXGlow and FFXDeath .bls programs (glow added,
 * not screened), over the client's blur (0x008bfe80, 0x008c1c20): FFXBox4
 * down to a quarter a side, then FFXGauss4 across and down
 * (screen_glow.comp.glsl).
 *
 * Built like SunShafts: the swapchain image is copied, three compute passes
 * make the blurred quarter, and the composite redraws the frame from the copy
 * in the overlay pass, ahead of the interface. The minimap is interface in the client, drawn after these; its
 * ellipse is left as it was.
 */
class ScreenEffects {
public:
    struct FrameInputs {
        float glow = 0.0f;   ///< LightParams.Glow, 0 for none
        float death = 0.0f;  ///< 0..1, how far the death desaturation is in
        /// The minimap's rect, framebuffer uv (x, y, w, h); w 0 for none.
        glm::vec4 keepRect{0.0f};
    };

    ScreenEffects() = default;
    ~ScreenEffects();
    ScreenEffects(const ScreenEffects&) = delete;
    ScreenEffects& operator=(const ScreenEffects&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx);
    void shutdown();

    /// Copy the finished frame and blur it. Outside any render pass, after the
    /// scene's last pass; the image is in PRESENT_SRC and goes back there.
    /// Returns whether composite() has anything to draw.
    bool record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                VkExtent2D extent, const FrameInputs& in);

    /// Redraw the frame with the effects, inside the overlay pass.
    void composite(VkCommandBuffer cmd, uint32_t frame);

private:
    static constexpr uint32_t MAX_FRAMES = 2;

    using Target = ScreenTarget;
    bool ensureTargets(VkExtent2D extent);
    void destroyTargets();
    bool ensureCompositePipeline();

    VkContext* ctx_ = nullptr;
    bool usable_ = false;

    Target frameCopy_[MAX_FRAMES];   // full size
    Target glowTemp_[MAX_FRAMES];    // a quarter a side: the Gauss's first pass (0xd4582c)
    Target glow_[MAX_FRAMES];        // a quarter a side: the box, then the blur (0xd45810)
    VkExtent2D smallExtent_{.width = 0, .height = 0};
    VkExtent2D sourceExtent_{.width = 0, .height = 0};
    bool drawThisFrame_[MAX_FRAMES] = {};
    FrameInputs inputs_[MAX_FRAMES];

    VkSampler sampler_ = VK_NULL_HANDLE;  // owned by the context cache

    VkDescriptorSetLayout blurSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    /// Per frame: the box (frame -> glow), across (glow -> temp), down (temp -> glow).
    VkDescriptorSet blurSets_[MAX_FRAMES][3] = {};
    VkDescriptorSet compositeSets_[MAX_FRAMES] = {};
};

} // namespace rendering
} // namespace wowee
