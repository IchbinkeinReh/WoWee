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

    // Sharp stars: an option in place of the client's stars model
    starField_ = std::make_unique<StarField>();
    if (!starField_->initialize(ctx, perFrameLayout)) {
        LOG_ERROR("Failed to initialize star field");
        return false;
    }
    starField_->setEnabled(false); // Off by default; the stars model is the client's

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

namespace {
Celestial::Frame celestialFrame(const Camera& camera, const SkyParams& params) {
    Celestial::Frame frame;
    frame.dayFraction = params.timeOfDay / 24.0f;
    frame.sunDir = params.sunDir;
    frame.moonDir = params.moonDir;
    frame.color = params.sunColor;
    frame.storm = daynight::stormBlend(params.weatherLight);
    frame.cameraForward = camera.getForward();
    frame.sunOcclusion = params.sunOcclusion;
    frame.moonOcclusion = params.moonOcclusion;
    frame.skyboxWeight = params.skyboxWeight;
    return frame;
}
}  // namespace

void SkySystem::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                        const Camera& camera, const SkyParams& params,
                        const std::function<void(VkCommandBuffer)>& drawStarModel) {
    if (!initialized_) {
        return;
    }

    // --- Stars ---
    // The client's are a model of their own, drawn first of the procedural
    // sky (0x007f09b0, 0x009abd50), and only with it; the caller decides
    // that and hands the draw in. Sharp stars, an option and off by default,
    // are a point field in their place - and drawn under a sky model too,
    // where M2Renderer drops that model's own star layer for them.
    const bool renderProceduralStars = debugSkyMode_ || proceduralStarsEnabled_;
    if (starField_) {
        starField_->setEnabled(renderProceduralStars);
        if (renderProceduralStars) {
            starField_->render(cmd, perFrameSet, params.timeOfDay);
        }
    }
    if (!renderProceduralStars && drawStarModel) drawStarModel(cmd);

    // Under a sky model up past 0.99 without LightSkybox flag 0x2 - or the
    // death model - the client draws none of its own sky: not the dome, the
    // sun, the moons or the clouds (0x007f09b0). What shows round the model
    // is the frame's clear colour.
    if (params.useOriginalSkybox) return;

    // --- The sun, the White Lady and the Blue Child ---
    // Under the dome, into the cleared sky (0x007f09b0 draws them before it).
    if (celestial_) {
        celestial_->render(cmd, perFrameSet, celestialFrame(camera, params));
    }

    // --- The dome, added on over the stars and the bodies (0x009acb00) ---
    if (skybox_) {
        // The glow's azimuth runs from the camera's heading (0x007f3920).
        skybox_->render(cmd, perFrameSet, params,
                        daynight::skyHighlightPhase(core::coords::renderToCanonical(camera.getForward())));
    }

    // --- Clouds: the light's cloud colours and cover, last (0x009acd40) ---
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

void SkySystem::updateGlare(const Camera& camera, const SkyParams& params) {
    if (initialized_ && celestial_) celestial_->updateGlare(celestialFrame(camera, params));
}

void SkySystem::renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (initialized_ && celestial_) celestial_->renderGlare(cmd, perFrameSet);
}

float SkySystem::getSunGlareDim() const {
    return celestial_ ? celestial_->getSunGlareDim() : 0.0f;
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
