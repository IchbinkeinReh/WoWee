#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_texture.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

/**
 * The client's sun and moons (Wow.exe 3.3.5a, DayNight).
 *
 * Three textured sprites 12 units from the eye on their own time curves
 * (0x007eecc0): Textures\sunCenter.blp, the White Lady (moon.blp) and the
 * Blue Child (moon02.blp), all in the light's ch9 at alpha 1 - storm, cut at
 * the horizon and faded over the last 0.4 units above it (0x007edbe0,
 * 0x007edee0, drawn by 0x009ac660). Over them one glare each for the sun and
 * the White Lady, Textures\sunGlare.blp and moonGlare.blp, added on: shown
 * by the hour, by whether the body is in view and by how little sky model
 * covers it, and growing as the camera turns to face it (0x007ee150,
 * 0x007ee230, 0x007ef6e0, 0x009ac400). No phases, no procedural discs.
 *
 * Pipeline layout:
 *   set 0  = perFrameLayout  (camera UBO)
 *   set 1  = one texture
 *   push   = CelestialPush   (direction and size, colour)
 */
class Celestial {
public:
    Celestial();
    ~Celestial();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();

    /// The five textures. Needs the asset manager, which arrives after the
    /// sky is built; nothing is drawn until it has run.
    void loadTextures(pipeline::AssetManager* assetManager);

    /// What one frame draws from.
    struct Frame {
        float dayFraction = 0.5f;            ///< 0..1
        glm::vec3 sunDir{0.0f, 0.0f, 1.0f};  ///< eye toward the sun, unit
        glm::vec3 moonDir{0.0f, 0.0f, -1.0f};
        glm::vec3 color{1.0f};               ///< ch9
        float storm = 0.0f;                  ///< min(1, 4 x weather)
        glm::vec3 cameraForward{0.0f, 1.0f, 0.0f};
        float sunOcclusion = 0.0f;           ///< 0 in view, 1 hidden
        float moonOcclusion = 0.0f;
        float skyboxWeight = 0.0f;           ///< the heaviest sky model up
        bool drawGlare = true;               ///< false for a reflection
    };

    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Frame& frame);

    /// Steps the glare toward what it should show (0x007ef6e0 by frame time).
    void update(float deltaTime) { deltaTime_ = deltaTime; }

    void setEnabled(bool enabled) { renderingEnabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const { return renderingEnabled_; }

private:
    struct CelestialPush {
        glm::vec4 dirSize;  // xyz = toward the body, w = sprite size (client units)
        glm::vec4 color;    // rgb, a
    };
    static_assert(sizeof(CelestialPush) == 32, "CelestialPush size mismatch");

    enum Tex { TEX_SUN = 0, TEX_MOON, TEX_BLUE_CHILD, TEX_SUN_GLARE, TEX_MOON_GLARE, TEX_COUNT };

    VkPipeline buildPipeline(VkDevice device,
                             const VkPipelineShaderStageCreateInfo& vertStage,
                             const VkPipelineShaderStageCreateInfo& fragStage,
                             bool additive);
    void createQuad();
    void destroyQuad();
    void destroyTextures();
    void drawSprite(VkCommandBuffer cmd, Tex tex, const glm::vec3& dir, float size,
                    const glm::vec4& color);

    VkContext*            vkCtx_          = nullptr;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;  // sprites, alpha blended
    VkPipeline            glarePipeline_  = VK_NULL_HANDLE;  // glare, added
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texSetLayout_   = VK_NULL_HANDLE;
    VkDescriptorPool      texPool_        = VK_NULL_HANDLE;
    VkDescriptorSet       texSets_[TEX_COUNT] = {};
    VkTexture             textures_[TEX_COUNT];
    VkBuffer              vertexBuffer_   = VK_NULL_HANDLE;
    VmaAllocation         vertexAlloc_    = VK_NULL_HANDLE;
    VkBuffer              indexBuffer_    = VK_NULL_HANDLE;
    VmaAllocation         indexAlloc_     = VK_NULL_HANDLE;

    bool renderingEnabled_ = true;
    float deltaTime_ = 0.0f;
    // How much of each glare shows, stepped toward its target each frame.
    float sunGlare_ = 0.0f;
    float moonGlare_ = 0.0f;
};

} // namespace rendering
} // namespace wowee
