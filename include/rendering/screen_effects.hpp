#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <cstdint>

#include "rendering/screen_target.hpp"
#include "rendering/vk_texture.hpp"

namespace wowee {
namespace rendering {

class VkContext;

/**
 * The client's full-screen effects (Wow.exe 3.3.5a), over the finished world
 * and under the interface, one at a time as ScreenEffect.dbc's row picks them
 * (0x004f7020; screen_effect::chooseRow says which row):
 *
 * - ffxGlow, "full screen glow effect", on by default (0x008bfe80; EffectGlow
 *   / PassGlow): FFXBox4 down to a quarter a side, FFXGauss4 across and down
 *   (screen_glow.comp.glsl), and FFXGlow - the frame mixed toward the blur by
 *   z, the squared blur added at LightParams.Glow. z is a third with the
 *   camera in liquid, or how drunk the player is (0x004f8770); in liquid
 *   FFXGlowWave reads both through a scrolling wave texture (0x008c2350).
 * - ffxDeath, "full screen death effect" (0x007ea260, 0x007e87b0): the
 *   glowed frame's luminance, (0x53, 0x93, 0xa8) laid over it by
 *   4 x lum x (1 - lum).
 * - ffxNetherWorld, "full screen nether world effect (for invisibility)"
 *   (0x007ea470): FFXNetherBlur streaks the frame along a drifting 6x6 field
 *   of angles, twice, into the quarter targets (screen_nether.comp.glsl), and
 *   FFXNetherCombine tints it toward (0.6, 0.6, 0.78) by a fade that reaches
 *   0.75 in as many seconds.
 * - ffxSpecial, "full screen test effect" (0x007ea5f0): a fog seeded from a
 *   noise texture creeps in from the screen's corners (FFXPropagateFog on a
 *   256x128 target, screen_fog.comp.glsl) and FFXFogCombine lays it over the
 *   frame, desaturated and brightened by the row's params.
 *
 * Built like SunShafts: the swapchain image is copied, compute passes make
 * what the effect reads, and the composite redraws the frame from the copy
 * in the overlay pass, ahead of the interface. The minimap is interface in
 * the client, drawn after these; its ellipse is left as it was.
 */
class ScreenEffects {
public:
    enum class Mode : uint32_t { Glow = 0, Death = 1, Nether = 2, Fog = 3 };

    struct FrameInputs {
        Mode mode = Mode::Glow;
        float glow = 0.0f;   ///< LightParams.Glow as the vertex alpha carries it
        float blend = 0.0f;  ///< the vertex blue: how far toward the blur
        bool wave = false;   ///< FFXGlowWave in FFXGlow's place
        uint32_t timeMs = 0; ///< the wave's scroll clock
        /// The minimap's rect, framebuffer uv (x, y, w, h); w 0 for none.
        glm::vec4 keepRect{0.0f};
        // Nether world: the grid's values, the shared angle and the fade.
        std::array<float, 36> netherValues{};
        float netherAngle = 0.0f;
        float netherFade = 0.0f;
        // Special fog: the seed colour (rgb), decay, noise row, combine terms.
        glm::vec3 fogColour{1.0f};
        float fogDecay = 0.0f;
        uint32_t fogNoiseRow = 0;
        float fogDesaturate = 0.0f;
        float fogBrighten = 0.0f;
    };

    ScreenEffects() = default;
    ~ScreenEffects();
    ScreenEffects(const ScreenEffects&) = delete;
    ScreenEffects& operator=(const ScreenEffects&) = delete;

    [[nodiscard]] bool initialize(VkContext* ctx);
    void shutdown();

    /// Copy the finished frame and run the effect's passes. Outside any
    /// render pass, after the scene's last pass; the image is in PRESENT_SRC
    /// and goes back there. Returns whether composite() has anything to draw.
    bool record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                VkExtent2D extent, const FrameInputs& in);

    /// Redraw the frame with the effects, inside the overlay pass.
    void composite(VkCommandBuffer cmd, uint32_t frame);

private:
    static constexpr uint32_t MAX_FRAMES = 2;
    static constexpr uint32_t kBlurSets = 4;  // box, across, down, frame -> temp

    using Target = ScreenTarget;
    bool ensureTargets(VkExtent2D extent);
    void destroyTargets();
    bool ensureCompositePipeline();
    bool createComputePipeline(const char* path, VkPipelineLayout layout, VkPipeline& out);
    bool ensureFogTargets();
    void writeCompositeSets();

    VkContext* ctx_ = nullptr;
    bool usable_ = false;

    Target frameCopy_[MAX_FRAMES];   // full size
    Target glowTemp_[MAX_FRAMES];    // a quarter a side: the Gauss's first pass (0xd4582c)
    Target glow_[MAX_FRAMES];        // a quarter a side: the box, then the blur (0xd45810)
    VkExtent2D smallExtent_{.width = 0, .height = 0};
    VkExtent2D sourceExtent_{.width = 0, .height = 0};
    bool drawThisFrame_[MAX_FRAMES] = {};
    FrameInputs inputs_[MAX_FRAMES];
    uint32_t fogSource_[MAX_FRAMES] = {};  ///< which fog target the composite reads

    /// The special fog's two targets, ping-ponged, kept from frame to frame.
    Target fog_[2];
    uint32_t fogLatest_ = 0;
    VkTexture fogNoise_;
    VkTexture wave_;

    VkSampler sampler_ = VK_NULL_HANDLE;  // owned by the context cache

    VkDescriptorSetLayout blurSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout netherLayout_ = VK_NULL_HANDLE;
    VkPipeline netherPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout fogSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout fogLayout_ = VK_NULL_HANDLE;
    VkPipeline fogPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    /// Per frame: the box (frame -> glow), across (glow -> temp), down
    /// (temp -> glow), and the nether's first streak (frame -> temp).
    VkDescriptorSet blurSets_[MAX_FRAMES][kBlurSets] = {};
    VkDescriptorSet compositeSets_[MAX_FRAMES] = {};
    /// The fog's steps: A -> B and B -> A.
    VkDescriptorSet fogSets_[2] = {};
};

} // namespace rendering
} // namespace wowee
