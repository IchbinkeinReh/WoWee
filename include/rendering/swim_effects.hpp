#pragma once

#include <unordered_map>
#include "rendering/vk_texture.hpp"
#include "rendering/water_ripple.hpp"
#include <array>
#include <optional>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class CameraController;
class WaterRenderer;
class M2Renderer;
class VkContext;

class SwimEffects {
public:
    SwimEffects();
    ~SwimEffects();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();
    /// The three pipelines both initialize() and recreatePipelines() need.
    void buildRipplePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);
    void buildBubblePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);
    void buildInsectPipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs);

    /// Select the render pass these particles will be recorded into. Water now
    /// draws after them in a pass of its own, so the spray has to move into that
    /// pass too or the water sheet is painted straight over it. Call before
    /// initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }
    void update(const Camera& camera, const CameraController& cc,
                const WaterRenderer& water, float deltaTime);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    void spawnFootSplash(const glm::vec3& footPos, float waterH);
    void setM2Renderer(M2Renderer* renderer) { m2Renderer = renderer; }
    /// The client's ripples (water_ripple.hpp): their two textures.
    void loadRippleTextures(pipeline::AssetManager* assets);
    /// Whether they show: not on the traced water.
    void setSurfaceRipplesShown(bool shown) { surfaceRipplesShown_ = shown; }
    /// Whether a unit's ripples are wanted at all this frame.
    [[nodiscard]] bool surfaceRipplesShown() const { return surfaceRipplesShown_; }
    /// A unit other than the player with its feet under `waterZ` this frame.
    void unitInWater(uint64_t guid, const glm::vec3& feet, float waterZ, bool moving, float speed, float facing,
                     float scale, float height, float deltaTime);

private:
    struct Particle {
        glm::vec3 position;
        glm::vec3 velocity;
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
    };

    struct InsectParticle {
        glm::vec3 position;
        glm::vec3 orbitCenter;  // vegetation position to orbit around
        float lifetime;
        float maxLifetime;
        float size;
        float alpha;
        float phase;       // random phase offset for erratic motion
        float orbitRadius;
        float orbitSpeed;
        float heightOffset; // height above plant
    };

    /// How far from the character another unit's ripples are made (yards).
    static constexpr float kUnitRippleRange = 150.0f;
    /// The surface ripples alive at once, the character's and every unit's
    /// within kUnitRippleRange together. Each lives about two thirds of a
    /// second and a unit makes one every 0.05-0.45 s (water_ripple::spawn),
    /// so this holds a hundred-odd units wading at once. Each is drawn with
    /// its own push constants, so nothing else is sized from it.
    static constexpr size_t kMaxSurfaceRipples = 2048;
    static constexpr int MAX_RIPPLE_PARTICLES = 200;
    static constexpr int MAX_BUBBLE_PARTICLES = 150;
    static constexpr int MAX_INSECT_PARTICLES = 50;

    std::vector<Particle> ripples;
    std::vector<Particle> bubbles;
    std::vector<InsectParticle> insects;

    // Vulkan objects
    VkContext* vkCtx = nullptr;
    M2Renderer* m2Renderer = nullptr;

    // Ripple pipeline + dynamic buffer
    VkPipeline ripplePipeline = VK_NULL_HANDLE;
    VkPipelineLayout ripplePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer rippleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation rippleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo rippleDynamicVBAllocInfo{};
    VkDeviceSize rippleDynamicVBSize = 0;

    // Bubble pipeline + dynamic buffer
    VkPipeline bubblePipeline = VK_NULL_HANDLE;
    VkPipelineLayout bubblePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer bubbleDynamicVB = VK_NULL_HANDLE;
    VmaAllocation bubbleDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo bubbleDynamicVBAllocInfo{};
    VkDeviceSize bubbleDynamicVBSize = 0;

    // Insect pipeline + dynamic buffer
    VkPipeline insectPipeline = VK_NULL_HANDLE;
    VkPipelineLayout insectPipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer insectDynamicVB = VK_NULL_HANDLE;
    VmaAllocation insectDynamicVBAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo insectDynamicVBAllocInfo{};
    VkDeviceSize insectDynamicVBSize = 0;

    // The client's ripples, one flat quad each (water_ripple.hpp).
    bool createSurfaceRipplePipeline();
    void updateSurfaceRipples(const CameraController& cc, const glm::vec3& charPos,
                              std::optional<float> waterH, float deltaTime);
    void renderSurfaceRipples(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    std::vector<water_ripple::Ripple> surfaceRipples_;
    float surfaceRippleDelay_ = 0.0f;
    float surfaceRippleLastDepth_ = 0.0f;
    struct UnitRipple {
        float delay = 0.0f;
        float lastDepth = 0.0f;
        float seen = 0.0f;  // rippleClock_ when last in water
    };
    float rippleClock_ = 0.0f;
    /// WOWEE_RIPPLE_DIAG: ripples refused for a full pool, and the most alive
    /// at once, since the last report.
    uint32_t surfaceRipplesDropped_ = 0;
    size_t surfaceRipplesPeak_ = 0;
    float surfaceRippleDiagClock_ = 0.0f;
    std::unordered_map<uint64_t, UnitRipple> unitRipples_;
    bool surfaceRipplesShown_ = false;
    VkPipeline surfaceRipplePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout surfaceRippleLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout surfaceRippleSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool surfaceRipplePool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> surfaceRippleSets_{};
    std::array<VkTexture, 2> surfaceRippleTextures_{};
    ::VkBuffer surfaceRippleQuad_ = VK_NULL_HANDLE;
    VmaAllocation surfaceRippleQuadAlloc_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;

    std::vector<float> rippleVertexData;
    std::vector<float> bubbleVertexData;
    std::vector<float> insectVertexData;

    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;

    float wadeSpawnAccum = 0.0f;
    float bubbleSpawnAccum = 0.0f;
    float insectSpawnAccum = 0.0f;

    void spawnBubble(const glm::vec3& pos, float waterH);
    void spawnInsect(const glm::vec3& vegPos);
};

} // namespace rendering
} // namespace wowee
