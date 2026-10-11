#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

#include "rendering/eye_adaptation.hpp"
#include "rendering/screen_target.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

class VkContext;

/**
 * The eye's adaptation, while the ray traced lighting is on
 * (eye_adaptation.hpp): the finished picture measured, the eye stepped
 * toward it, and the picture scaled by the exposure that gives, under the
 * interface.
 *
 * Measured where the sun shafts take their copy, the frame finished and out
 * of its passes before the overlay pass: the middle of the swapchain image -
 * where the world is, not the interface's bars and frames - blitted down to
 * a few thousand pixels and copied to a buffer the CPU reads two frames
 * later, when that slot comes round again and its fence has been waited on.
 * The few thousand pixels' mean takes microseconds; the exposure is then one
 * full-screen multiply in the overlay pass, ahead of the interface.
 *
 * With the lighting off nothing is recorded or drawn, and the picture is
 * exactly what it was.
 */
class EyeAdaptationPass {
public:
    EyeAdaptationPass() = default;
    ~EyeAdaptationPass();
    EyeAdaptationPass(const EyeAdaptationPass&) = delete;
    EyeAdaptationPass& operator=(const EyeAdaptationPass&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx);
    void shutdown();

    /// Take this frame's measurement and step the eye by `dt` seconds with
    /// the last one this slot holds. Outside any render pass, the swapchain
    /// image finished and in PRESENT_SRC, as SunShafts::record. With
    /// `active` false the eye is forgotten and nothing is recorded.
    void record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage, VkExtent2D extent, bool active,
                float dt);

    /// Scale the picture by the exposure. Inside the overlay pass, viewport
    /// set, before the interface. Draws nothing at an exposure of one.
    void composite(VkCommandBuffer cmd);

    [[nodiscard]] float exposure() const { return exposure_; }
    [[nodiscard]] float adaptedLog2() const { return adaptedLog2_; }

private:
    static constexpr uint32_t MAX_FRAMES = 2;
    /// The measured picture's size: some two thousand pixels.
    static constexpr uint32_t kWidth = 64;
    static constexpr uint32_t kHeight = 32;

    bool ensureCompositePipeline();
    void forget();

    VkContext* ctx_ = nullptr;
    bool usable_ = false;
    bool bgra_ = false;

    ScreenTarget small_[MAX_FRAMES];
    AllocatedBuffer readback_[MAX_FRAMES];
    bool measured_[MAX_FRAMES] = {};

    bool adapting_ = false;
    float adaptedLog2_ = 0.0f;
    float exposure_ = 1.0f;
    eye_adaptation::Params params_{};
    float logTimer_ = 0.0f;
    bool log_ = false;

    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;
};

} // namespace rendering
} // namespace wowee
