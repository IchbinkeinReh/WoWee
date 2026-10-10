#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "rendering/rt_gi_volume.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

class RtScene;

/**
 * Multi-bounce diffuse light from a volume of irradiance probes around the
 * camera (DDGI, Majercik et al. 2019), for RtLighting's Full mode.
 *
 * Two cascades of 32x32x16 probes, the far one four times as coarse, scroll
 * with the camera (rt_gi_volume.hpp). Each frame a share of them - every
 * probe that has just come into the grid, and of the rest one in `period` -
 * traces 64 rays into RtScene. A hit is lit by the sun with its own shadow
 * ray and by what the volume itself held there last frame, so each update
 * adds a bounce and the light converges on every bounce there is. The rays
 * are blended into each probe's octahedral irradiance tile and into its
 * distance moments, which the readers use to tell whether a probe can see
 * the point they shade (rt_gi.glsli).
 *
 * The surfaces read the atlas at set 0 binding 4 and do not take the
 * irradiance as their ambient: they scale the zone's flat ambient by how it
 * compares with what an open field gathers (rt_lighting.glsli), so the
 * Light.dbc colours stay what the artists set where nothing is in the way.
 *
 * Owned and driven by RtLighting, which shares its per-slot RtPassData block
 * with these passes (the block's gi* tail).
 */
class RtGiProbes {
public:
    /// The tail of RtPassData (rt_common.glsli) these passes read.
    struct PassData {
        glm::vec4 volume[gi::kCascades];
        glm::vec4 volumeMod[gi::kCascades];
        glm::vec4 prevVolume[gi::kCascades];
        glm::vec4 prevVolumeMod[gi::kCascades];
        glm::mat4 rotation;
        glm::vec4 sky;
        glm::vec4 ground;
        glm::vec4 rayLength;
    };

    /// What the surfaces need for the per-frame block (GPUPerFrameData's gi*).
    struct ConsumerData {
        glm::vec4 volume[gi::kCascades]{};
        glm::vec4 volumeMod[gi::kCascades]{};
        glm::vec4 sky{0.0f};
        glm::vec4 ground{0.0f};
        glm::vec4 params{0.0f};
    };

    /// Tuning, read once from the environment (see rt_gi_probes.cpp).
    struct Config {
        float spacing = 3.0f;        // WOWEE_GI_SPACING: the near cascade, yards
        uint32_t period = 4;         // WOWEE_GI_UPDATE_PERIOD: frames between a probe's updates
        float hysteresis = 0.95f;    // WOWEE_GI_HYSTERESIS
        float strength = 1.0f;       // WOWEE_GI_STRENGTH
        float maxRatio = 2.0f;       // WOWEE_GI_MAX_RATIO
        float groundAlbedo = 0.3f;   // WOWEE_GI_GROUND_ALBEDO
        float chroma = 0.15f;        // WOWEE_GI_CHROMA: how much of the bounce's tint shows
        float normalize = 1.6f;      // WOWEE_GI_NORMALIZE: the largest lift (rt_gi_exposure), 1 off
        int debug = 0;               // WOWEE_GI_DEBUG
    };

    RtGiProbes() = default;
    ~RtGiProbes();
    RtGiProbes(const RtGiProbes&) = delete;
    RtGiProbes& operator=(const RtGiProbes&) = delete;

    void initialize(VkContext* ctx, RtScene* scene);
    void shutdown();

    /// Create the atlas and buffers. With the device idle: the atlas is
    /// cleared and put in GENERAL with a submit of its own.
    bool createResources();
    /// Free them. With the device idle.
    void destroyResources();
    [[nodiscard]] bool hasResources() const { return atlas_.image != VK_NULL_HANDLE; }

    [[nodiscard]] VkImageView atlasView() const { return atlas_.imageView; }
    /// For tools that read the volume back (tools/rt_probe.cpp). GENERAL.
    [[nodiscard]] VkImage atlasImage() const { return atlas_.image; }

    /// Work out this frame's grid and list, and the block's gi* tail. `sky`
    /// is the radiance of the open sky (the flat ambient), `ground` that of
    /// open ground.
    void beginFrame(uint32_t slot, const glm::vec3& camera, uint64_t frame, const glm::vec3& sky,
                    const glm::vec3& ground, PassData& out);
    /// Record the trace and the update. Outside a render pass, after the scene
    /// update. `ubo` is the slot's RtPassData, `gbuf` the view of the
    /// tracer's G-buffer this slot builds (for recordExposure). Leaves the
    /// atlas written by compute, for the caller's barrier toward its readers.
    void record(VkCommandBuffer cmd, uint32_t slot, VkBuffer ubo, VkDeviceSize uboSize,
                VkImageView gbuf);
    /// Record the view's lift (rt_gi_exposure), once this frame's G-buffer
    /// is built and a barrier has made it and the update visible to compute.
    void recordExposure(VkCommandBuffer cmd, uint32_t slot);

    [[nodiscard]] ConsumerData consumerData() const;
    [[nodiscard]] const Config& config() const { return config_; }
    /// How many probes the last recorded frame updated.
    [[nodiscard]] uint32_t lastUpdateCount() const { return listCount_; }

private:
    bool createPipelines();
    void writeSet(uint32_t slot, VkBuffer ubo, VkDeviceSize uboSize, VkImageView gbuf);

    VkContext* ctx_ = nullptr;
    RtScene* scene_ = nullptr;
    Config config_;

    AllocatedImage atlas_{};
    AllocatedBuffer rays_{};
    AllocatedBuffer lists_[MAX_FRAMES_IN_FLIGHT]{};
    VkDescriptorSet sets_[MAX_FRAMES_IN_FLIGHT]{};
    bool setWritten_[MAX_FRAMES_IN_FLIGHT]{};
    VkImageView setGbuf_[MAX_FRAMES_IN_FLIGHT]{};
    bool recordedThisFrame_ = false;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline trace_ = VK_NULL_HANDLE;
    VkPipeline update_ = VK_NULL_HANDLE;
    VkPipeline exposure_ = VK_NULL_HANDLE;
    bool pipelinesTried_ = false;
    bool pipelinesOk_ = false;

    // The grid the atlas holds (after the last recorded update) and the one
    // this frame writes.
    glm::ivec3 gridMin_[gi::kCascades]{};
    bool haveGrid_ = false;
    glm::ivec3 nextGridMin_[gi::kCascades]{};
    std::vector<uint32_t> list_;
    uint32_t listCount_ = 0;
    bool frameBegun_ = false;
    glm::vec3 sky_{0.0f}, ground_{0.0f};
    glm::vec3 nextSky_{0.0f}, nextGround_{0.0f};
};

} // namespace rendering
} // namespace wowee
