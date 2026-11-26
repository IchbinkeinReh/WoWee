#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

#include "rendering/eye_adaptation.hpp"
#include "rendering/scene_depth_view.hpp"
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
 * The few thousand pixels' mean takes microseconds.
 *
 * The exposure is then laid over the frame in the overlay pass, ahead of the
 * interface and after the shafts and the screen effects. Not as a plain
 * multiply: the frame is 8-bit and its highlights already clipped, and white
 * times an exposure under one is grey - the low sun's glow was a flat grey
 * disc. The frame as it stands then is copied (prepareComposite, between two
 * overlay passes), a compute pass marks where the copy is clipped white, one
 * texel per cell of pixels, and the copy is redrawn under the exposure let
 * go toward one round those cells (eye_adaptation::exposeFrame): the
 * burnt-out sky stays white, its glare falls off softly, and everywhere else
 * it is the multiply it was. Only the sky's white is marked, by the scene's
 * depth: a surface drawn white - a floor under a fire's light and the glow -
 * is exposed as the rest of the world.
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

    /// Whether there is an exposure to lay over this frame: false at an
    /// exposure of one, when nothing is copied or drawn.
    [[nodiscard]] bool wantsComposite() const;

    /// The scene's depth, as its last pass left it (as SunShafts::FrameInputs
    /// has it): what tells the sky from what is drawn. Without an image every
    /// clipped pixel counts.
    struct SceneDepth {
        VkImage image = VK_NULL_HANDLE;
        bool multisampled = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        /// Changes whenever the scene's targets are made again.
        uint64_t generation = 0;
    };

    /// Copy the frame as it stands - the overlay pass that drew the shafts
    /// over it ended, the image in PRESENT_SRC - for composite() to read, and
    /// mark its clipped sky by `depth`. Outside any render pass; leaves the
    /// image in PRESENT_SRC and the depth in its layout. False when there is
    /// nothing to draw or the copy cannot be made.
    [[nodiscard]] bool prepareComposite(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                                        VkExtent2D extent, const SceneDepth& depth);

    /// Redraw the picture from that copy under the exposure. Inside the
    /// overlay pass, viewport set, before the interface; only after
    /// prepareComposite returned true for this frame.
    void composite(VkCommandBuffer cmd, uint32_t frame);

    /// How far and how fast the eye adapts (eye_adaptation::paramsFor).
    void setParams(const eye_adaptation::Params& params) { params_ = params; }

    [[nodiscard]] float exposure() const { return exposure_; }
    [[nodiscard]] float adaptedLog2() const { return adaptedLog2_; }

private:
    static constexpr uint32_t MAX_FRAMES = 2;
    /// The measured picture's size: some two thousand pixels.
    static constexpr uint32_t kWidth = 64;
    static constexpr uint32_t kHeight = 32;

    bool ensureCompositePipeline();
    bool ensureCopies(VkExtent2D extent);
    void destroyCopies();
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

    /// The frame as the shafts left it, full size, one a slot; and where it
    /// is clipped white, one texel per cell_ x cell_ pixels.
    ScreenTarget frameCopy_[MAX_FRAMES];
    ScreenTarget mask_[MAX_FRAMES];
    /// The mask blurred wide (r) beside the mask itself (g), at its size,
    /// which the composite reads (eye_adaptation::blurMask).
    ScreenTarget blur_[MAX_FRAMES];
    VkExtent2D copyExtent_{};
    VkExtent2D maskExtent_{};
    uint32_t cell_ = 0;
    VkSampler sampler_ = VK_NULL_HANDLE;        // the context's cache owns both
    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout maskSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet compositeSets_[MAX_FRAMES] = {};
    VkDescriptorSet maskSets_[MAX_FRAMES] = {};
    VkDescriptorSet blurSets_[MAX_FRAMES] = {};
    VkPipelineLayout maskLayout_ = VK_NULL_HANDLE;
    VkPipeline maskPipeline_ = VK_NULL_HANDLE;    // single-sampled depth, or none
    VkPipeline maskPipelineMs_ = VK_NULL_HANDLE;  // multisampled depth
    /// The depth view, and what each slot's mask and blur sets were last
    /// given for binding 2: the serial of the depth view, or 0 for the
    /// frame's copy standing in where there is no depth.
    SceneDepthView depthView_;
    uint64_t depthSetSerial_[MAX_FRAMES] = {};
    bool depthSetWritten_[MAX_FRAMES] = {};
    bool depthSampleable_ = false;

    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;
};

} // namespace rendering
} // namespace wowee
