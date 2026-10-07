#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;
struct SkyParams;

/**
 * The client's clouds (Wow.exe 3.3.5a, DayNight): a dome of 12 rows by 16
 * segments on the sky sphere (0x007f20e0) carrying a 128-texel noise texture
 * cut at the light's cloud cover (float band 3) and coloured by its cloud
 * channels ch10-ch12, lit from the sun by day and the moon by night
 * (0x007efd00, 0x007efae0). The texture is evaluated per pixel in
 * clouds.frag.glsl rather than built on the CPU.
 *
 * Pipeline layout:
 *   set 0 = perFrameLayout  (camera UBO - view, projection, etc.)
 *   push  = CloudPush       (4 x vec4 = 64 bytes)
 */
class Clouds {
public:
    Clouds();
    ~Clouds();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    /// The pipeline both initialize() and recreatePipelines() need.
    void buildPipeline(VkDevice device,
                       const VkPipelineShaderStageCreateInfo& vertStage,
                       const VkPipelineShaderStageCreateInfo& fragStage);

    /**
     * Render clouds using DBC-driven colors and sun lighting.
     * @param cmd         Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0, camera UBO)
     * @param params      Sky parameters with DBC colors and sun direction
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const SkyParams& params);

    /**
     * Update cloud animation: the noise evolves in place, it does not drift
     * (0x007efd00 steps the noise's third axis with time).
     */
    void update(float deltaTime);

    // --- Enable / disable ---
    void setEnabled(bool enabled) { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled_; }

    /// The cover the clouds were last drawn with, 0..1 (float band 3).
    [[nodiscard]] float getDensity() const { return density_; }

private:
    // Push constant block - must match clouds.frag.glsl
    struct CloudPush {
        glm::vec4 sunLit;  // rgb = ch10, w = glow
        glm::vec4 shade;   // rgb = ch11, w = coverage threshold (noise byte)
        glm::vec4 base;    // rgb = ch12, w = the light's height over the texture
        glm::vec4 light;   // xy = sun or moon texel, z = noise time
    };
    static_assert(sizeof(CloudPush) == 64, "CloudPush size mismatch");

    struct CloudVertex {
        glm::vec3 pos;      // on the sky sphere, relative to the eye
        glm::vec3 uvAlpha;  // xy = texture uv, z = row alpha
    };

    void generateMesh();
    void createBuffers();
    void destroyBuffers();

    // Vulkan objects
    VkContext*       vkCtx_          = nullptr;
    VkPipeline       pipeline_       = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkBuffer         vertexBuffer_   = VK_NULL_HANDLE;
    VmaAllocation    vertexAlloc_    = VK_NULL_HANDLE;
    VkBuffer         indexBuffer_    = VK_NULL_HANDLE;
    VmaAllocation    indexAlloc_     = VK_NULL_HANDLE;

    // Mesh data (CPU side, used during initialization only)
    std::vector<CloudVertex> vertices_;
    std::vector<uint32_t>    indices_;
    int                      indexCount_ = 0;

    bool  enabled_   = true;
    float density_   = 0.0f;
    float noiseTime_ = 0.0f;

    // 0x007f20e0: 16 segments round, the rows from kCloudDomeRows.
    static constexpr int SEGMENTS = 16;
};

} // namespace rendering
} // namespace wowee
