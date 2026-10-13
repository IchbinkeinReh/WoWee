#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "rendering/rt_gi_probes.hpp"
#include "rendering/rt_mover.hpp"
#include "rendering/rt_point_lights.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

class RtScene;

/**
 * Ray traced sun shadows, ambient occlusion and multi-bounce diffuse light for
 * the forward renderer.
 *
 * The diffuse light is a probe volume around the camera (RtGiProbes), updated
 * in Full mode alongside the per-pixel terms below and read by the surfaces
 * at their own position and normal. The rest of this comment is the
 * per-pixel sun and occlusion.
 *
 * There is no G-buffer to trace from, so the pass runs after the opaque
 * geometry, on the depth it left: rebuild normals from depth, trace one
 * sample per term per pixel at half resolution, accumulate over frames with
 * reprojection, and smooth with an edge-aware filter. The surface shaders
 * read the result in the *next* frame, reprojecting their own position into
 * the frame the result was traced in (rt_lighting.glsli). Where that misses -
 * a surface just revealed, or one that moved - they fall back to the shadow
 * map and the flat ambient, so the one frame of latency only ever shows as
 * the old lighting, never as a hole.
 *
 * What RtScene holds casts: terrain, buildings and doodads, and as dynamic
 * instances the characters, what they carry and what moves
 * (character_renderer_rt.cpp, RtScene's dynamic top level).
 *
 * The point lights of the frame - torches, braziers, lamps, the omni lights
 * of buildings (rt_point_lights.hpp) - are traced in the same pass: their
 * light unshadowed into an image of its own, the fraction of it that arrives
 * beside the sun's visibility (rt_trace.comp.glsl). The surfaces add their
 * product to their light. With the pass off nothing reads either, and the
 * world is lit as the original lights it, by none of them.
 *
 * The results are double-buffered by frame slot. Slot s writes its outputs
 * after its opaque pass; the per-frame set of slot s binds the outputs of the
 * other slot, which is the frame before it.
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

    /// Creates the sampler and the neutral image the per-frame sets bind while
    /// the pass is off. Pipelines are built on first use.
    bool initialize(VkContext* ctx, RtScene* scene);
    void shutdown();

    void setMode(Mode mode) { mode_ = mode; }
    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool active() const { return mode_ != Mode::Off && pipelinesOk_; }

    /// Bring the images to the size the scene is rendered at, and the probe
    /// volume into being or out of it with the mode. Call between frames,
    /// before the per-frame sets are used. Returns true when the views
    /// changed and the per-frame sets must be written again.
    bool prepare(VkExtent2D sceneExtent);

    /// The immutable sampler for per-frame bindings 3 to 5.
    [[nodiscard]] VkSampler sampler() const { return linearSampler_; }
    /// What per-frame set `slot` binds at 3 (sun, AO, point light share,
    /// distance), 4 (the probe atlas, rt_gi.glsli) and 5 (the point lights
    /// unshadowed).
    [[nodiscard]] VkImageView lightViewForSlot(uint32_t slot) const;
    [[nodiscard]] VkImageView giViewForSlot(uint32_t slot) const;
    [[nodiscard]] VkImageView pointViewForSlot(uint32_t slot) const;

    /// The most point lights one frame traces; the rest of a crowded view's
    /// are the least of them (rt_lights::selectLights).
    static constexpr uint32_t kMaxPointLights = 128;
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
        /// What moves on its own this frame, most important (nearest) first;
        /// past kMaxMovers are dropped. The surfaces do not take a mover's
        /// texels of the result for what stood behind it a frame earlier
        /// (rtLightAt, rt_lighting.glsli).
        const std::vector<RtMover>* movers = nullptr;
    };
    /// Must match RT_MAX_MOVERS in rt_movers.glsli. A G-buffer texel carries
    /// its mover as 4 x (index + 1) beside its 2x2 sub-pixel in a half float,
    /// which holds whole numbers exactly to 2048.
    static constexpr uint32_t kMaxMovers = 500;

    /// Record the pass. Outside a render pass, after the opaque geometry.
    /// The depth image must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL and is
    /// returned to it.
    void record(VkCommandBuffer cmd, VkImage sceneDepth, VkExtent2D sceneExtent, bool depthMsaa,
                const FrameInputs& in);

    /// What the surface shaders need to find last frame's result, for the
    /// per-frame block: the view-projection it was traced with, the camera
    /// position, and params (x = mode, 0 when there is no usable result);
    /// and the probe volume as the atlas holds it (gi.params.x 0 when off).
    struct ConsumerData {
        glm::mat4 viewProj{1.0f};
        glm::vec4 cameraPos{0.0f};
        glm::vec4 params{0.0f};
        RtGiProbes::ConsumerData gi;
    };
    [[nodiscard]] ConsumerData consumerData() const;

private:
    struct Target {
        AllocatedImage img{};
    };
    struct Slot {
        Target gbuf, hist, out;
        Target point;              // the point lights unshadowed (rt_trace.comp)
        AllocatedBuffer ubo{};
        AllocatedBuffer lights{};  // rt_lights::PointLightGPU[kMaxPointLights], mapped
        AllocatedBuffer movers{};  // RtMoverGPU[kMaxMovers], mapped
        VkDescriptorSet sets[3] = {};  // one per filter pass; set 0 also runs the rest
        VkImage boundDepth = VK_NULL_HANDLE;
        bool wroteImages = false;
    };

    bool createPipelines();
    bool createImages(VkExtent2D traceExtent);
    void destroyImages();
    void writeSlotSets(uint32_t slot);
    /// The frame's movers into the slot's list, those on screen; how many.
    uint32_t writeMovers(Slot& s, const FrameInputs& in);
    VkImageView depthViewFor(VkImage image, bool msaa);

    VkContext* ctx_ = nullptr;
    RtScene* scene_ = nullptr;
    Mode mode_ = Mode::Off;
    bool pipelinesTried_ = false;
    bool pipelinesOk_ = false;

    VkExtent2D sceneExtent_{};
    VkExtent2D traceExtent_{};
    Slot slots_[MAX_FRAMES_IN_FLIGHT];
    Target raw_, tmp_;
    RtGiProbes probes_;
    AllocatedImage neutral_{};
    bool imagesNeedInit_ = false;

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
    bool depthViewMsaa_ = false;

    // Counted by prepare(); a result is usable only in the frame right after
    // the one that traced it, since that is whose slot the sets bind.
    uint64_t frameCounter_ = 0;
    uint64_t resultFrame_ = 0;
    glm::mat4 resultViewProj_{1.0f};
    glm::vec3 resultCameraPos_{0.0f};
    Mode resultMode_ = Mode::Off;
    glm::mat4 prevViewProj_{1.0f};
    glm::vec3 prevCameraPos_{0.0f};
    bool havePrev_ = false;
};

} // namespace rendering
} // namespace wowee
