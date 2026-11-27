#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

#include "rendering/rt_point_lights.hpp"
#include "rendering/scene_depth_view.hpp"
#include "rendering/screen_target.hpp"
#include "rendering/volumetric_light_math.hpp"

namespace wowee {
namespace rendering {

class VkContext;

/**
 * Light seen in the air itself: shafts of sunlight - or moonlight at night -
 * falling through the gaps in a canopy, a doorway or a window, and the glow
 * round every torch, brazier, lamp and lava pool in the air about it. The
 * graphics option "Volumetric light"; the original client has nothing like
 * it, so it is off unless chosen, and off it records nothing at all.
 *
 * It sits beside the mist ("Light shafts and mist", VolumetricFog) rather
 * than in it. The mist is a medium over the whole view, built before the
 * scene and read by every surface, and carries its cells over from frame to
 * frame; this is the light alone, in clear air, gathered after the scene
 * against the scene's own depth, and smoothed over the last few frames. With the
 * mist on, the sun's shafts are the mist's and this adds the moon's and the
 * lamps'.
 *
 * Each frame, outside every pass once the scene is finished, like the sun
 * shafts:
 *
 *   depth   the scene's depth down to the march's size, the nearest of the
 *           pixels each texel covers (volumetric_light_depth.comp.glsl);
 *   march   every texel's ray to that depth: a fixed number of samples of the
 *           sun's (moon's) light in the near air, each lit only where the sky
 *           body itself reaches it - the shadow map, or a ray query to it with
 *           the ray traced lighting - and a fixed number more inside the reach
 *           of each lamp the ray crosses, each lit only where the lamp can be
 *           seen from it - a traced ray with the ray traced lighting, a few
 *           steps against the scene's depth without (volumetric_light.comp.glsl);
 *   blur    a 5x5 blur kept to like depths (volumetric_light_blur.comp.glsl);
 *   history this frame's light blended into the last frames' at 0.2, found
 *           again where the same air was last frame and held to this
 *           frame's light round it (volumetric_light_temporal.comp.glsl;
 *           U23, the user allowed the light the last ~5 frames);
 *
 * and in the overlay pass, ahead of the interface and the eye's exposure,
 * the result is screened over the picture (volumetric_light_composite.frag),
 * the minimap's disc left out where it was drawn with the scene.
 *
 * Faint by design (volumetric_light::scatterFor): the sky body's light counts
 * only in lit air with shade beside it (volumetric_light::shaftRadius), so it
 * shows as shafts where something shades part of the air - strongest toward
 * the sun - and never as a veil over the open sky or a plain;
 * a lamp's as a soft glow round its flame in its own colour, fainter by day.
 *
 * The lamps are the ones the ray traced lighting shades with, gathered the
 * same way (rt_lights): a model's light only while its flames or its glowing
 * surface show; not a building's own light, which stands for its rooms'
 * painted light and no flame. The sun's light only while the sun is up and the moon's only
 * after it has set and while she is up; neither indoors but through an
 * opening, since each sample asks whether the sky body reaches it.
 *
 * Its cost is bounded by the quality (volumetric_light::budgetFor): a half or
 * a quarter of the screen a side, a fixed number of samples a ray, at most so
 * many lamps. Double-buffered per frame in flight.
 */
class VolumetricLight {
public:
    using Quality = volumetric_light::Quality;

    /// How each sample is asked whether light reaches it.
    enum class Occlusion : int { None = 0, ShadowMap = 1, DepthSteps = 1, Traced = 2 };

    struct FrameInputs {
        /// False draws nothing this frame.
        bool draw = false;
        /// The matrices the scene was drawn with this frame.
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 cameraPos{0.0f};

        /// Toward the sun or the moon, unit length; its share of light (0
        /// gathers none) and colour, and the forward lobe's share (1).
        glm::vec3 bodyDir{0.0f, 0.0f, 1.0f};
        float bodyShare = 0.0f;
        glm::vec3 bodyColor{0.0f};
        float lobe = 1.0f;
        /// ShadowMap or Traced; None leaves the sky body out.
        Occlusion bodyOcclusion = Occlusion::None;
        /// Scattered per yard, the air's thickness folded in.
        float bodyScatter = 0.0f;
        float lampScatter = 0.0f;

        /// The lamps, most important first; past the quality's count are
        /// dropped.
        std::vector<rt_lights::PointLightGPU> lamps;
        /// DepthSteps or Traced.
        Occlusion lampOcclusion = Occlusion::DepthSteps;

        /// The traced scene, when either occlusion is Traced: its set, its
        /// layout, which tracer, whether it holds anything.
        VkDescriptorSet sceneSet = VK_NULL_HANDLE;
        VkDescriptorSetLayout sceneLayout = VK_NULL_HANDLE;
        bool hardware = false;
        bool sceneHasContent = false;

        /// The renderer's per-frame set (its UBO: the shadow cascades), and
        /// this frame slot's shadow map when it is readable, else none.
        VkDescriptorSet perFrameSet = VK_NULL_HANDLE;
        VkImageView shadowView = VK_NULL_HANDLE;

        /// The scene's depth, as SunShafts::FrameInputs has it.
        VkImage depthImage = VK_NULL_HANDLE;
        bool depthMultisampled = false;
        VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        uint64_t depthGeneration = 0;

        /// Where the minimap is on screen when it was drawn with the scene
        /// (uv x, y, w, h; w 0 for none): the light is left off its disc,
        /// which is interface and no air (T435), as the screen's glow
        /// leaves it out (ScreenEffects::FrameInputs::keepRect).
        glm::vec4 keepRect{0.0f};

        /// The debug view "VL only" (dbgvlonly): the light alone over black
        /// (volumetric_light::debugViewColor).
        bool debugView = false;
    };

    VolumetricLight() = default;
    ~VolumetricLight();
    VolumetricLight(const VolumetricLight&) = delete;
    VolumetricLight& operator=(const VolumetricLight&) = delete;

    /// Set 0 of the passes is the renderer's per-frame layout.
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();

    /// Off frees its images; a quality builds them at the next record.
    void setQuality(Quality q);
    [[nodiscard]] Quality quality() const { return quality_; }
    /// The pipelines built: the option can work here.
    [[nodiscard]] bool usable() const { return usable_; }

    /// Build this frame's light. Outside any render pass, after the scene's
    /// last one has closed. Returns whether composite() has something to add.
    bool record(VkCommandBuffer cmd, uint32_t frame, VkExtent2D extent, const FrameInputs& in);

    /// Screen it over the picture - or, in the debug view, draw it over
    /// black in its place. Inside the overlay pass, viewport set.
    void composite(VkCommandBuffer cmd, uint32_t frame);
    /// This frame slot's light is the debug view: composited after the
    /// eye's exposure, so the view is the light as it is.
    [[nodiscard]] bool debugViewDrawn(uint32_t frame) const;

private:
    static constexpr uint32_t MAX_FRAMES = 2;

    bool ensureTargets(VkExtent2D extent);
    void destroyTargets();
    bool ensureCompositePipeline();
    /// The march with the traced scene at set 2, for this layout and tracer.
    VkPipeline tracedPipeline(VkDescriptorSetLayout sceneLayout, bool hardware);
    void destroyTracedPipelines();

    VkContext* ctx_ = nullptr;
    bool usable_ = false;
    Quality quality_ = Quality::Off;

    ScreenTarget lowDepth_[MAX_FRAMES];
    ScreenTarget march_[MAX_FRAMES];
    ScreenTarget blurred_[MAX_FRAMES];
    /// Two histories, not one a frame slot: each frame reads the one the
    /// frame before wrote and writes the other (historyWrite_).
    ScreenTarget history_[2];
    uint32_t historyWrite_ = 0;
    /// The last record drew the light into these targets: the next may
    /// build on it (volumetric_light::historyUsable).
    bool historyValid_ = false;
    glm::mat4 prevViewProj_{1.0f};
    glm::vec3 prevCamPos_{0.0f};
    uint32_t frameNumber_ = 0;  // moves the march's pattern, 0..1023
    bool debugView_[MAX_FRAMES] = {};
    ScreenTarget dummyShadow_;  // 1x1, read when there is no shadow map
    VkExtent2D targetExtent_{.width = 0, .height = 0};
    VkExtent2D sourceExtent_{.width = 0, .height = 0};
    Quality targetQuality_ = Quality::Off;
    bool drawThisFrame_[MAX_FRAMES] = {};

    VkSampler linearSampler_ = VK_NULL_HANDLE;  // the context cache's
    VkSampler pointSampler_ = VK_NULL_HANDLE;
    SceneDepthView depthView_;

    VkBuffer paramsUBO_[MAX_FRAMES] = {};
    VmaAllocation paramsAlloc_[MAX_FRAMES] = {};
    void* paramsMapped_[MAX_FRAMES] = {};

    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;  // the renderer's
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout baseLayout_ = VK_NULL_HANDLE;
    VkPipeline depthPipeline_ = VK_NULL_HANDLE;
    VkPipeline depthPipelineMs_ = VK_NULL_HANDLE;
    VkPipeline marchPipeline_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;
    VkPipeline temporalPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout tracedSceneLayout_ = VK_NULL_HANDLE;  // the scene's, which these were built for
    VkPipelineLayout tracedLayout_ = VK_NULL_HANDLE;
    VkPipeline tracedSw_ = VK_NULL_HANDLE;
    VkPipeline tracedHw_ = VK_NULL_HANDLE;
    bool tracedSwFailed_ = false;
    bool tracedHwFailed_ = false;

    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkPipeline debugPipeline_ = VK_NULL_HANDLE;  // the debug view: no blend
    VkFormat compositeFormat_ = VK_FORMAT_UNDEFINED;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[MAX_FRAMES] = {};
    VkDescriptorSet compositeSets_[MAX_FRAMES] = {};
    glm::vec4 keepRect_[MAX_FRAMES] = {};
};

} // namespace rendering
} // namespace wowee
