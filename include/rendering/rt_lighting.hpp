#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "rendering/generation_keyed_cache.hpp"
#include "rendering/rt_gi_probes.hpp"
#include "rendering/rt_point_lights.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

class RtScene;

/**
 * Ray traced sun shadows, ambient occlusion, point lights and multi-bounce
 * diffuse light, on a deferred lighting path (docs/deferred_rt_design.md).
 *
 * The opaque surfaces draw as the client draws them, each computing its own
 * colour in its own shader, and in the same draw write a G-buffer beside it
 * (VkContext::getGbufferRenderPass, rt_gbuffer.glsli): how that colour
 * depends on the traced terms, which it drew neutral. Right after them, in
 * the same frame, this traces from the depth and that G-buffer - one sample
 * per term per pixel at half resolution, accumulated over frames by
 * reprojecting the texels' own positions with the camera's motion, smoothed
 * with an edge-aware filter - and a full-screen pass applies the result to
 * the colour (rt_apply.glsli). What is drawn after it - the blended batches,
 * a unit drawn see-through - reads the same result at its own position
 * (rt_lighting.glsli). Nothing is read a frame late.
 *
 * The diffuse light is a probe volume around the camera (RtGiProbes), updated
 * in Full mode beside the per-pixel terms and read by the surfaces at their
 * own position and normal.
 *
 * What RtScene holds casts: terrain, buildings and doodads, and as dynamic
 * instances the characters, what they carry and what moves
 * (character_renderer_rt.cpp, RtScene's dynamic top level).
 *
 * The point lights of the frame - torches, braziers, lamps, the omni lights
 * of buildings (rt_point_lights.hpp) - are traced in the same pass: their
 * light unshadowed into an image of its own, the fraction of it that arrives
 * beside the sun's visibility (rt_trace.comp.glsl). With the pass off nothing
 * reads either, and the world is lit as the original lights it, by none of
 * them.
 *
 * The history is double-buffered by frame slot: slot s accumulates over the
 * other slot's, which is the frame before it.
 */
class RtLighting {
public:
    /// Persisted as an index; add at the end. Full adds the probe volume's
    /// multi-bounce light to ShadowsAO.
    enum class Mode : int { Off = 0, Shadows = 1, ShadowsAO = 2, Full = 3 };

    RtLighting() = default;
    ~RtLighting();
    RtLighting(const RtLighting&) = delete;
    RtLighting& operator=(const RtLighting&) = delete;

    /// Creates the samplers and the neutral image the per-frame sets bind
    /// while the pass is off. Pipelines are built on first use.
    bool initialize(VkContext* ctx, RtScene* scene);
    void shutdown();

    void setMode(Mode mode) { mode_ = mode; }
    [[nodiscard]] Mode mode() const { return mode_; }
    /// Trace every scene pixel instead of one per 2x2 block (Settings > Ray
    /// Tracing). About four times the trace's cost, and every pixel finds
    /// its own surface - thin foliage, poles, edges - instead of falling back
    /// to the look without the tracing. The images follow on the next
    /// prepare().
    void setFullResolution(bool full) { fullResolution_ = full; }
    [[nodiscard]] bool fullResolution() const { return fullResolution_; }
    [[nodiscard]] bool active() const { return mode_ != Mode::Off && pipelinesOk_; }
    /// What the first traced frame would otherwise do over seconds: the
    /// pipelines, and every mesh of the scene built and uploaded. For the
    /// loading screen; nothing while the tracing is off.
    void warmUp();
    /// Whether a frame can be drawn through the G-buffer pass now: active,
    /// with its images, and the device able to (VkContext::getGbufferRenderPass).
    [[nodiscard]] bool ready() const;

    /// Bring the images to the size and sample count the scene is rendered
    /// at, and the probe volume into being or out of it with the mode. Call
    /// between frames, before the per-frame sets are used. Returns true when
    /// the views changed and the per-frame sets must be written again.
    bool prepare(VkExtent2D sceneExtent, VkSampleCountFlagBits samples);

    /// The immutable sampler for per-frame bindings 3 to 5.
    [[nodiscard]] VkSampler sampler() const { return linearSampler_; }
    /// What per-frame set `slot` binds at 3 (sun, AO, point light share,
    /// distance), 4 (the probe atlas, rt_gi.glsli) and 5 (the point lights
    /// unshadowed): the result the frame of that slot traces, which what it
    /// draws after the trace reads.
    [[nodiscard]] VkImageView lightViewForSlot(uint32_t slot) const;
    [[nodiscard]] VkImageView giViewForSlot(uint32_t slot) const;
    [[nodiscard]] VkImageView pointViewForSlot(uint32_t slot) const;

    /// The most point lights one frame can trace, what the lights buffer
    /// holds; how many it does is the Debug Lights limit, at most this
    /// (rt_lights::lightLimit). The rest of a crowded view's are the least
    /// of them (rt_lights::selectLights).
    static constexpr uint32_t kMaxPointLights = static_cast<uint32_t>(rt_lights::kMaxLightLimit);
    /// This frame's point lights, most important first, for the next
    /// record(). Past kMaxPointLights are dropped.
    void setPointLights(const std::vector<rt_lights::PointLight>& lights);
    [[nodiscard]] uint32_t pointLightCount() const { return static_cast<uint32_t>(pointLights_.size()); }
    [[nodiscard]] VkImageView neutralView() const { return neutral_.imageView; }
    /// The image slot `slot` writes, in GENERAL layout, at the trace size.
    /// For tools that read the result back (tools/rt_probe.cpp).
    [[nodiscard]] VkImage lightImageWrittenBy(uint32_t slot) const { return slots_[slot].out.img.image; }
    [[nodiscard]] VkExtent2D traceExtent() const { return traceExtent_; }
    [[nodiscard]] const RtGiProbes& probes() const { return probes_; }

    /// The generation of the scene's colour and depth targets, which changes
    /// whenever any of them is destroyed or made again (a swapchain rebuild,
    /// an anti-aliasing change, FXAA or an upscaler switched on or off).
    /// Everything made over the old targets - the G-buffer and apply
    /// framebuffers, the view of the depth - is let go then, and nothing is
    /// found by the new targets' handles that was made for the old ones
    /// (generation_keyed_cache.hpp). Call each frame before
    /// gbufferFramebuffer().
    void setSceneTargetsGeneration(uint64_t generation);

    /// The G-buffer pass's framebuffer over the scene's own colour (its MSAA
    /// image under MSAA) and depth views; made on first use.
    VkFramebuffer gbufferFramebuffer(VkImageView colour, VkImageView depth);

    struct FrameInputs {
        glm::mat4 viewProj;
        glm::vec3 cameraPos;
        glm::vec3 sunDir;      // toward the sun
        glm::vec3 sunColor;
        glm::vec3 skyColor;    // what the flat ambient term stands for
        bool sunUp = true;
        /// Where the sun's shadow map draws the units' shadows: the sun's
        /// rays pass the units by within unitMapRadius of the centre (0 for
        /// no map), the map giving their shadows in this frame's pose.
        glm::vec3 unitMapCenter{0.0f};
        float unitMapRadius = 0.0f;
        /// The Debug Graphics strengths, as the per-frame block has them
        /// (rtTune, lightTune), for the apply pass: the shadow darkness, the
        /// traced sun shadow, the occlusion, the point lights, the baked
        /// lamps' shadows, the point lights' shadows; and the debug view.
        float shadowStrength = 0.65f;
        float sunShadow = 1.0f;
        float occlusion = 1.0f;
        float pointLights = 1.0f;
        float bakedLampShadow = 1.0f;
        float pointShadow = 1.0f;
        int debugView = 0;
        /// Inside: what of a baked colour shows where no point light reaches,
        /// and the light at which all of it does (rtInteriorShade).
        float interiorBase = 1.0f;
        float interiorFullAt = 1.0f;
        /// The most light the traced lights add (rtCapLight); 0 no limit.
        float maxLight = 0.0f;
    };

    /// The scene the G-buffer pass drew: its depth image, still in
    /// DEPTH_STENCIL_ATTACHMENT_OPTIMAL (it is returned to it), and its
    /// colour as attachment 0 - the MSAA image under MSAA - as the G-buffer
    /// pass left it, which the apply pass draws into.
    struct SceneTarget {
        VkImage depthImage = VK_NULL_HANDLE;
        VkImageView colourView = VK_NULL_HANDLE;
        VkExtent2D extent{};
    };

    /// Record the trace and the apply pass, outside a render pass, after the
    /// G-buffer pass and before anything blended.
    void record(VkCommandBuffer cmd, const SceneTarget& target, const FrameInputs& in);

    /// What the surface shaders need for the per-frame block: params.x the
    /// mode when this frame is traced (traced), 0 otherwise; and the probe
    /// volume as the atlas holds it (gi.params.x 0 when off).
    struct ConsumerData {
        glm::vec4 params{0.0f};
        RtGiProbes::ConsumerData gi;
    };
    [[nodiscard]] ConsumerData consumerData(bool traced) const;

private:
    struct Target {
        AllocatedImage img{};
    };
    struct Slot {
        Target gbuf, hist, out;
        Target point;              // the point lights unshadowed (rt_trace.comp)
        AllocatedBuffer ubo{};
        AllocatedBuffer lights{};  // rt_lights::PointLightGPU[kMaxPointLights], mapped
        VkDescriptorSet sets[3] = {};  // one per filter pass; set 0 also runs the rest
        uint64_t setsGeneration = 0;   // the views they were written for
    };

    bool createPipelines();
    bool createApplyPipeline();
    bool createImages(VkExtent2D traceExtent);
    void destroyImages();
    bool createGbuffer(VkExtent2D extent, VkSampleCountFlagBits samples);
    void destroyGbuffer();
    void writeSlotSets(uint32_t slot);
    VkImageView depthViewFor(VkImage image);
    VkFramebuffer applyFramebuffer(VkImageView colour);
    void recordApply(VkCommandBuffer cmd, const SceneTarget& target, Slot& s);

    VkContext* ctx_ = nullptr;
    RtScene* scene_ = nullptr;
    Mode mode_ = Mode::Off;
    bool fullResolution_ = false;
    bool pipelinesTried_ = false;
    bool pipelinesOk_ = false;

    VkExtent2D traceExtent_{};
    Slot slots_[MAX_FRAMES_IN_FLIGHT];
    Target raw_, tmp_;
    RtGiProbes probes_;
    AllocatedImage neutral_{};

    // The opaque surfaces' G-buffer (VkContext::kGbufferFormat), at the
    // scene's size and sample count, and the passes over it.
    AllocatedImage gbuf0_{}, gbuf1_{};
    VkExtent2D gbufExtent_{};
    VkSampleCountFlagBits gbufSamples_ = VK_SAMPLE_COUNT_1_BIT;
    // Keyed by the scene's view handles, which a rebuild can hand out again
    // for new images: emptied whenever the scene's targets change
    // (setSceneTargetsGeneration).
    GenerationKeyedCache<std::pair<VkImageView, VkImageView>, VkFramebuffer> gbufferFramebuffers_;
    GenerationKeyedCache<VkImageView, VkFramebuffer> applyFramebuffers_;
    /// Destroys the framebuffers once no frame in flight can use them.
    void releaseFramebuffersLater(std::vector<VkFramebuffer> framebuffers);
    VkRenderPass applyRenderPass_ = VK_NULL_HANDLE;
    VkPipeline apply_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits applySamples_ = VK_SAMPLE_COUNT_1_BIT;
    /// Counts the views the sets bind - the G-buffer, the depth - so a slot
    /// rewrites its sets when they changed.
    uint64_t viewsGeneration_ = 1;

    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkSampler pointSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline prepare_ = VK_NULL_HANDLE;
    VkPipeline prepareMs_ = VK_NULL_HANDLE;
    VkPipeline trace_ = VK_NULL_HANDLE;
    VkPipeline temporal_ = VK_NULL_HANDLE;
    VkPipeline filter_ = VK_NULL_HANDLE;

    std::vector<rt_lights::PointLightGPU> pointLights_;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;

    // Counted by prepare(): the history is usable only in the frame right
    // after the one that wrote it, since that is whose slot holds it.
    uint64_t frameCounter_ = 0;
    uint64_t resultFrame_ = 0;
    glm::mat4 prevViewProj_{1.0f};
    glm::vec3 prevCameraPos_{0.0f};
};

} // namespace rendering
} // namespace wowee
