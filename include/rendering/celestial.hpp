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
 * 0x007edee0, drawn alpha blended by 0x009ac660 under the sky dome, which is
 * added on over them). And one glare each for the sun and the White Lady,
 * Textures\sunGlare.blp and moonGlare.blp, added on over the finished world
 * with no depth test: shown by the hour, by whether the body is in view and
 * by how little sky model covers it, and growing as the camera turns to face
 * it (0x007ee150, 0x007ee230, 0x007ef6e0, 0x009ac400). The sun's glare also
 * darkens the world's light (0x007816f0). No phases, no procedural discs.
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
    };

    /// The three bodies, drawn with the procedural sky (0x007f09b0).
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Frame& frame);

    /// Steps the glare toward what it should show (0x007ef6e0 by frame time)
    /// and works out this frame's glare quads. Once a frame, whether or not
    /// the procedural sky is drawn (0x007f0870 is called on its own).
    void updateGlare(const Frame& frame);
    /// The glare, added on over the finished world with no depth test
    /// (0x009ac400). Reads only what updateGlare left.
    void renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    /// The pass the glare is drawn in: the scene's (null) or, when water is
    /// drawn in a continuation pass, that one, since the client draws the
    /// glare after the water (0x004f8ea0 calls 0x007f0870 last of the world).
    /// Takes effect on the next recreateGlarePipeline().
    void setGlareTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        glareTargetPass_ = pass;
        glareTargetSamples_ = samples;
    }
    void recreateGlarePipeline();
    /// Whether the glare pipeline was built for a pass other than the one it
    /// is now set to.
    [[nodiscard]] bool glarePipelineStale() const;
    /// How much the sun's glare takes off the world's ambient and direct
    /// light (daynight::sunGlareWorldDim, 0x007816f0).
    [[nodiscard]] float getSunGlareDim() const { return sunGlareDim_; }

    void update(float deltaTime) { deltaTime_ = deltaTime; }

    void setEnabled(bool enabled) { renderingEnabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const { return renderingEnabled_; }

private:
    struct CelestialPush {
        glm::vec4 dirSize;  // xyz = toward the body, w = sprite size (client units)
        glm::vec4 color;    // rgb, a
        glm::vec4 params;   // x = 1 for a body (cut and faded at the horizon), 0 for glare
    };
    static_assert(sizeof(CelestialPush) == 48, "CelestialPush size mismatch");

    enum Tex { TEX_SUN = 0, TEX_MOON, TEX_BLUE_CHILD, TEX_SUN_GLARE, TEX_MOON_GLARE, TEX_COUNT };

    VkPipeline buildPipeline(VkDevice device,
                             const VkPipelineShaderStageCreateInfo& vertStage,
                             const VkPipelineShaderStageCreateInfo& fragStage,
                             bool glare);
    void createQuad();
    void destroyQuad();
    void destroyTextures();
    void drawSprite(VkCommandBuffer cmd, Tex tex, const glm::vec3& dir, float size,
                    const glm::vec4& color, bool body);
    void bindQuad(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet perFrameSet);

    VkContext*            vkCtx_          = nullptr;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;  // sprites, alpha blended
    VkPipeline            glarePipeline_  = VK_NULL_HANDLE;  // glare, added, no depth test
    VkRenderPass          glareTargetPass_ = VK_NULL_HANDLE;  // null: the scene pass
    VkSampleCountFlagBits glareTargetSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPass          glareBuiltFor_ = VK_NULL_HANDLE;  // the pass glarePipeline_ was built for
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
    float glareDiagTimer_ = 0.0f;  // WOWEE_GLARE_DIAG: seconds to the next log
    // How much of each glare shows, stepped toward its target each frame.
    float sunGlare_ = 0.0f;
    float moonGlare_ = 0.0f;
    struct GlareQuad {
        glm::vec3 dir{0.0f, 0.0f, 1.0f};
        float size = 0.0f;
        glm::vec4 color{0.0f};
    };
    GlareQuad glare_[2];
    bool glareReady_ = false;
    float sunGlareDim_ = 0.0f;
};

} // namespace rendering
} // namespace wowee
