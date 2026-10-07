#pragma once

#include <functional>
#include <memory>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;
class Skybox;
class Celestial;
class StarField;
class Clouds;
class LensFlare;
class LightingManager;

/**
 * Sky rendering parameters (extracted from LightingManager)
 */
struct SkyParams {
    // Sun/moon positioning
    glm::vec3 directionalDir{0.0f, -1.0f, 0.3f};
    /// From the eye toward the sun and the moon, unit length: the client's own
    /// curves for them (LightingParams::sunDir, moonDir; 0x007eecc0).
    glm::vec3 sunDir{0.0f, 0.0f, 1.0f};
    glm::vec3 moonDir{0.0f, 0.0f, -1.0f};
    /// ch9: the sun's and moons' sprite colour (0x007f3230).
    glm::vec3 sunColor{1.0f, 1.0f, 0.9f};

    // Sky colors (for skybox tinting/blending)
    glm::vec3 skyTopColor{0.5f, 0.7f, 1.0f};
    glm::vec3 skyMiddleColor{0.7f, 0.85f, 1.0f};
    glm::vec3 skyBand1Color{0.9f, 0.95f, 1.0f};
    glm::vec3 skyBand2Color{1.0f, 0.98f, 0.9f};
    glm::vec3 skySmogColor{0.7f, 0.7f, 0.7f};   // ch6
    glm::vec3 skyFogColor{0.7f, 0.7f, 0.7f};    // ch7: the horizon, the fog's colour
    /// The dawn and dusk glow on the dome's bands, 0..1: the time curve
    /// times LightParams.HighlightSky (0x007f0530).
    float skyHighlight = 0.0f;

    /// The clouds' colours: ch10 sunlit, ch11 by thinness, ch12 base
    /// (0x007efae0).
    glm::vec3 cloudSunColor{1.0f, 1.0f, 1.0f};
    glm::vec3 cloudShadeColor{0.5f, 0.5f, 0.5f};
    glm::vec3 cloudBaseColor{0.3f, 0.3f, 0.3f};

    // Atmospheric effects
    float cloudDensity = 0.0f;      // float band 3, the clouds' coverage
    float weatherIntensity = 0.0f;  // 0-1, any kind of weather
    /// What the weather hands the light (Weather::lightValue, 0x00784850);
    /// the storm sets blend in by min(1, 4 x this) (0x007f3920).
    float weatherLight = 0.0f;
    // How much of the line from the eye to the sun is blocked, 0 clear to 1
    // solid. The lens flare had no such input: it asked only whether the sun
    // was in front of the camera and on screen, so a hillside, a building or a
    // ceiling between the two changed nothing and the flare hung over the
    // terrain that was covering it.
    float sunOcclusion = 0.0f;
    /// The same for the White Lady, for her glare.
    float moonOcclusion = 0.0f;
    /// The heaviest sky model up, 0..1: the glare fades under it
    /// (0x007ef6e0).
    float skyboxWeight = 0.0f;

    // Time
    float timeOfDay = 12.0f;    // 0-24 hours
    float gameTime = -1.0f;     // Server game time, hours since midnight (-1 = use fallback)

    // Skybox selection (future: from LightSkybox.dbc)
    uint32_t skyboxModelId = 0;
    bool useOriginalSkybox = false; // Original camera-centered client M2 is active
};

/**
 * Unified sky rendering system
 *
 * Coordinates skybox (authoritative), celestial bodies (sun + 2 moons),
 * and fallback procedural stars. Driven by lighting system data.
 *
 * Architecture:
 * - Skybox is PRIMARY (includes baked stars from M2 models)
 * - Celestial renders sun + White Lady + Blue Child
 * - StarField is DEBUG/FALLBACK only (disabled when skybox has stars)
 */
class SkySystem {
public:
    SkySystem();
    ~SkySystem();

    /**
     * Initialize sky system components.
     * @param ctx            Vulkan context (required for Vulkan renderers)
     * @param perFrameLayout Descriptor set layout for set 0 (camera UBO)
     */
    bool initialize(VkContext* ctx = nullptr, VkDescriptorSetLayout perFrameLayout = VK_NULL_HANDLE);
    void shutdown();

    /**
     * Update sky system (time, moon phases, etc.)
     */
    void update(float deltaTime);

    /**
     * Render complete sky.
     * @param cmd         Active Vulkan command buffer
     * @param perFrameSet Per-frame descriptor set (set 0, camera UBO)
     * @param camera      Camera for legacy sub-renderers (lens flare, etc.)
     * @param params      Sky parameters from lighting system
     * @param drawStarModel Draws the client's stars model, called first,
     *                    before the sun, the moons, the dome and the clouds,
     *                    which is where 0x007f09b0 draws it. Empty for none.
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                const Camera& camera, const SkyParams& params,
                const std::function<void(VkCommandBuffer)>& drawStarModel = {});

    /// Steps the sun's and the White Lady's glare once a frame, whether or
    /// not the procedural sky is drawn (0x007f0870). Main thread.
    void updateGlare(const Camera& camera, const SkyParams& params);
    /// The glare, over the finished world with no depth test (0x009ac400).
    void renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    /// How much the sun's glare darkens the world's light (0x007816f0).
    [[nodiscard]] float getSunGlareDim() const;

    /**
     * Enable/disable procedural stars (DEBUG/FALLBACK)
     * Default: OFF (stars come from skybox)
     */
    void setProceduralStarsEnabled(bool enabled) { proceduralStarsEnabled_ = enabled; }
    [[nodiscard]] bool isProceduralStarsEnabled() const { return proceduralStarsEnabled_; }

    /**
     * Enable/disable debug sky mode (forces procedural stars even with skybox)
     */
    void setDebugSkyMode(bool enabled) { debugSkyMode_ = enabled; }
    [[nodiscard]] bool isDebugSkyMode() const { return debugSkyMode_; }

    /**
     * Get sun position in world space (for lens flare, shadows, etc.)
     */
    [[nodiscard]] glm::vec3 getSunPosition(const SkyParams& params) const;

    /// The sun's and moons' textures, once there is an asset manager.
    void loadTextures(pipeline::AssetManager* assetManager);

    // Component accessors (for direct control if needed)
    [[nodiscard]] Skybox*    getSkybox()    const { return skybox_.get(); }
    [[nodiscard]] Celestial* getCelestial() const { return celestial_.get(); }
    [[nodiscard]] StarField* getStarField() const { return starField_.get(); }
    [[nodiscard]] Clouds*    getClouds()    const { return clouds_.get(); }
    [[nodiscard]] LensFlare* getLensFlare() const { return lensFlare_.get(); }

private:
    std::unique_ptr<Skybox>    skybox_;      // Authoritative sky
    std::unique_ptr<Celestial> celestial_;   // Sun + 2 moons
    std::unique_ptr<StarField> starField_;   // Fallback procedural stars
    std::unique_ptr<Clouds>    clouds_;      // Cloud layer
    std::unique_ptr<LensFlare> lensFlare_;   // Sun lens flare

    bool proceduralStarsEnabled_ = false;
    bool debugSkyMode_ = false;
    bool initialized_ = false;
};

} // namespace rendering
} // namespace wowee
