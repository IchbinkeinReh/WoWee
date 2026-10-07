#include "rendering/sky_system.hpp"
#include "rendering/day_night.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_context.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

SkySystem::SkySystem() = default;

SkySystem::~SkySystem() {
    shutdown();
}

bool SkySystem::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    if (initialized_) {
        LOG_WARNING("SkySystem already initialized");
        return true;
    }

    LOG_INFO("Initializing sky system");

    // Skybox (Vulkan)
    skybox_ = std::make_unique<Skybox>();
    if (!skybox_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize skybox");
        return false;
    }

    // Celestial bodies - sun + 2 moons (Vulkan)
    celestial_ = std::make_unique<Celestial>();
    if (!celestial_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize celestial bodies");
        return false;
    }

    // Procedural stars - fallback / debug (Vulkan)
    starField_ = std::make_unique<StarField>();
    if (!starField_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize star field");
        return false;
    }
    starField_->setEnabled(false); // Off by default; skybox is authoritative

    // Clouds (Vulkan)
    clouds_ = std::make_unique<Clouds>();
    if (!clouds_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize clouds");
        return false;
    }

    // Lens flare (Vulkan)
    lensFlare_ = std::make_unique<LensFlare>();
    if (!lensFlare_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize lens flare");
        return false;
    }

    initialized_ = true;
    LOG_INFO("Sky system initialized successfully");
    return true;
}

void SkySystem::shutdown() {
    if (!initialized_) {
        return;
    }

    LOG_INFO("Shutting down sky system");

    if (lensFlare_)  lensFlare_->shutdown();
    if (clouds_)     clouds_->shutdown();
    if (starField_)  starField_->shutdown();
    if (celestial_)  celestial_->shutdown();
    if (skybox_)     skybox_->shutdown();

    lensFlare_.reset();
    clouds_.reset();
    starField_.reset();
    celestial_.reset();
    skybox_.reset();

    initialized_ = false;
}

void SkySystem::update(float deltaTime) {
    if (!initialized_) {
        return;
    }

    if (skybox_)    skybox_->update(deltaTime);
    if (celestial_) celestial_->update(deltaTime);
    if (starField_) starField_->update(deltaTime);
    if (clouds_)    clouds_->update(deltaTime);
}

void SkySystem::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                        const Camera& camera, const SkyParams& params) {
    if (!initialized_) {
        return;
    }

    // --- Skybox (authoritative sky gradient, DBC-driven colors) ---
    if (skybox_) {
        skybox_->render(cmd, perFrameSet, params);
    }

    // Original client sky M2s supply their own celestial bodies, clouds, and
    // nebula layers. The caller draws that model over the gradient underlay.

    // --- Procedural stars ---
    // Not only a fallback any more: with sharp stars on, the sky model keeps
    // its clouds and planets while M2Renderer drops its star layer, and these
    // are drawn in that layer's place. So the original-skybox path continues
    // here rather than returning, and skyboxHasStars stops meaning "do not".
    bool renderProceduralStars = false;
    if (debugSkyMode_) {
        renderProceduralStars = true;
    } else if (proceduralStarsEnabled_) {
        renderProceduralStars = true;
    } else if (!params.useOriginalSkybox) {
        renderProceduralStars = !params.skyboxHasStars;
    }

    if (starField_) {
        starField_->setEnabled(renderProceduralStars);
        if (renderProceduralStars) {
            starField_->render(cmd, perFrameSet, params.timeOfDay, params.cloudDensity);
        }
    }

    // Under a sky model up past 0.99 without LightSkybox flag 0x2 - or the
    // death model - the client draws none of its own sky (0x007f09b0).
    if (params.useOriginalSkybox) return;

    // --- The sun, the White Lady and the Blue Child, and their glare ---
    if (celestial_) {
        Celestial::Frame frame;
        frame.dayFraction = params.timeOfDay / 24.0f;
        frame.sunDir = params.sunDir;
        frame.moonDir = params.moonDir;
        frame.color = params.sunColor;
        frame.storm = daynight::stormBlend(params.weatherIntensity);
        frame.cameraForward = camera.getForward();
        frame.sunOcclusion = params.sunOcclusion;
        frame.moonOcclusion = params.moonOcclusion;
        frame.skyboxWeight = params.skyboxWeight;
        frame.drawGlare = params.drawGlare;
        celestial_->render(cmd, perFrameSet, frame);
    }

    // --- Clouds: the light's cloud colours and cover (0x007efd00) ---
    if (clouds_) {
        clouds_->render(cmd, perFrameSet, params);
    }

    // --- Lens flare: not the client's, off unless chosen ---
    if (lensFlare_) {
        glm::vec3 sunPos = getSunPosition(params);
        lensFlare_->render(cmd, camera, sunPos, params.timeOfDay,
                           0.0f, params.cloudDensity,
                           params.weatherIntensity, params.sunOcclusion);
    }
}

glm::vec3 SkySystem::getSunPosition(const SkyParams& params) const {
    // The client's sun curve (0x007eecc0), not the light's direction: the two
    // share a side of the sky but not a height. Below the horizon it stays
    // below - see sun_direction.hpp for what mirroring it up once cost.
    const float lenSq = glm::dot(params.sunDir, params.sunDir);
    if (lenSq < 1e-8f) return glm::vec3(0.0f, 0.0f, 800.0f);
    return params.sunDir * glm::inversesqrt(lenSq) * 800.0f;
}

void SkySystem::loadTextures(pipeline::AssetManager* assetManager) {
    if (celestial_) celestial_->loadTextures(assetManager);
}

} // namespace rendering
} // namespace wowee
