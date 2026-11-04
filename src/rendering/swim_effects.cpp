#include "rendering/swim_effects.hpp"
#include "rendering/depth_convention.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

static std::mt19937& rng() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return gen;
}

static float randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng());
}

SwimEffects::SwimEffects() = default;
SwimEffects::~SwimEffects() { shutdown(); }

/// The ripple pipeline: the rings that spread where a swimmer breaks the
/// surface.
///
/// initialize() and recreatePipelines() described these three identically,
/// so each was stated twice in one file.
void SwimEffects::buildRipplePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    ripplePipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, depth::kNearer)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(ripplePipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

/// The bubble pipeline: what rises from a swimmer under the surface.
void SwimEffects::buildBubblePipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    bubblePipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, depth::kNearer)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(bubblePipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

/// The insect pipeline: the midges that hover over still water.
void SwimEffects::buildInsectPipeline(
        VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
        const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& binding,
        const std::vector<VkVertexInputAttributeDescription>& attrs) {
    const std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    insectPipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(false, false, depth::kNearer)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetSamples_)
        .setLayout(insectPipelineLayout)
        .setRenderPass(targetPass_)
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx->getPipelineCache());
}

bool SwimEffects::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing swim effects");

    vkCtx = ctx;
    perFrameLayout_ = perFrameLayout;
    VkDevice device = vkCtx->getDevice();

    // Fall back to drawing inside the scene pass when no target has been
    // selected, which is the arrangement that predates water having its own.
    if (targetPass_ == VK_NULL_HANDLE) {
        targetPass_ = vkCtx->getImGuiRenderPass();
        targetSamples_ = vkCtx->getMsaaSamples();
    }

    // ---- Vertex input: pos(vec3) + size(float) + alpha(float) = 5 floats, stride = 20 bytes ----
    VkVertexInputBindingDescription binding = tightVertexBinding(5 * sizeof(float));
    std::vector<VkVertexInputAttributeDescription> attrs = positionPlusTwoFloatsAttrs();


    // ---- Ripple pipeline ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_ripple.vert.spv", "assets/shaders/swim_ripple.frag.spv", "swim_ripple");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        ripplePipelineLayout = createPipelineLayout(device, {perFrameLayout}, {});
        if (ripplePipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple pipeline layout");
            return false;
        }

        buildRipplePipeline(device, vertStage, fragStage, binding, attrs);


        if (ripplePipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple pipeline");
            return false;
        }
    }

    // ---- Bubble pipeline ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_bubble.vert.spv", "assets/shaders/swim_bubble.frag.spv", "swim_bubble");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        bubblePipelineLayout = createPipelineLayout(device, {perFrameLayout}, {});
        if (bubblePipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble pipeline layout");
            return false;
        }

        buildBubblePipeline(device, vertStage, fragStage, binding, attrs);


        if (bubblePipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble pipeline");
            return false;
        }
    }

    // ---- Insect pipeline (dark point sprites) ----
    {
        auto shaders = loadShaderPair(device, "assets/shaders/swim_ripple.vert.spv", "assets/shaders/swim_insect.frag.spv", "swim_ripple");
        if (!shaders) return false;
        const auto& vertStage = shaders.vertStage;
        const auto& fragStage = shaders.fragStage;

        insectPipelineLayout = createPipelineLayout(device, {perFrameLayout}, {});
        if (insectPipelineLayout == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create insect pipeline layout");
            return false;
        }

        // Depth test disabled - insects are screen-space sprites that must always
        // render above the water surface regardless of scene geometry.
        buildInsectPipeline(device, vertStage, fragStage, binding, attrs);


        if (insectPipeline == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create insect pipeline");
            return false;
        }
    }

    // ---- Create dynamic mapped vertex buffers ----
    rippleDynamicVBSize = MAX_RIPPLE_PARTICLES * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), rippleDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        rippleDynamicVB = buf.buffer;
        rippleDynamicVBAlloc = buf.allocation;
        rippleDynamicVBAllocInfo = buf.info;
        if (rippleDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create ripple dynamic vertex buffer");
            return false;
        }
    }

    bubbleDynamicVBSize = MAX_BUBBLE_PARTICLES * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), bubbleDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        bubbleDynamicVB = buf.buffer;
        bubbleDynamicVBAlloc = buf.allocation;
        bubbleDynamicVBAllocInfo = buf.info;
        if (bubbleDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create bubble dynamic vertex buffer");
            return false;
        }
    }

    insectDynamicVBSize = MAX_INSECT_PARTICLES * 5 * sizeof(float);
    {
        AllocatedBuffer buf = createBuffer(vkCtx->getAllocator(), insectDynamicVBSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        insectDynamicVB = buf.buffer;
        insectDynamicVBAlloc = buf.allocation;
        insectDynamicVBAllocInfo = buf.info;
        if (insectDynamicVB == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to create insect dynamic vertex buffer");
            return false;
        }
    }

    ripples.reserve(MAX_RIPPLE_PARTICLES);
    bubbles.reserve(MAX_BUBBLE_PARTICLES);
    insects.reserve(MAX_INSECT_PARTICLES);
    rippleVertexData.reserve(MAX_RIPPLE_PARTICLES * 5);
    bubbleVertexData.reserve(MAX_BUBBLE_PARTICLES * 5);
    insectVertexData.reserve(MAX_INSECT_PARTICLES * 5);

    LOG_INFO("Swim effects initialized");
    return true;
}

void SwimEffects::shutdown() {
    if (vkCtx) {
        VkDevice device = vkCtx->getDevice();
        VmaAllocator allocator = vkCtx->getAllocator();

        destroy(device, ripplePipeline);
        destroy(device, ripplePipelineLayout);
        destroy(allocator, rippleDynamicVB, rippleDynamicVBAlloc);

        for (auto& texture : surfaceRippleTextures_) texture.destroy(device, allocator);
        destroy(allocator, surfaceRippleQuad_, surfaceRippleQuadAlloc_);
        destroy(device, surfaceRipplePipeline_);
        destroy(device, surfaceRippleLayout_);
        destroy(device, surfaceRipplePool_);
        destroy(device, surfaceRippleSetLayout_);
        surfaceRippleSets_.fill(VK_NULL_HANDLE);
        surfaceRipples_.clear();

        destroy(device, bubblePipeline);
        destroy(device, bubblePipelineLayout);
        destroy(allocator, bubbleDynamicVB, bubbleDynamicVBAlloc);

        destroy(device, insectPipeline);
        destroy(device, insectPipelineLayout);
        destroy(allocator, insectDynamicVB, insectDynamicVBAlloc);
    }

    vkCtx = nullptr;
    ripples.clear();
    bubbles.clear();
    insects.clear();
}

void SwimEffects::recreatePipelines() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    destroy(device, surfaceRipplePipeline_);

    // Fall back to drawing inside the scene pass when no target has been
    // selected, which is the arrangement that predates water having its own.
    if (targetPass_ == VK_NULL_HANDLE) {
        targetPass_ = vkCtx->getImGuiRenderPass();
        targetSamples_ = vkCtx->getMsaaSamples();
    }

    // Destroy old pipelines (NOT layouts)
    destroy(device, ripplePipeline);
    destroy(device, bubblePipeline);
    destroy(device, insectPipeline);

    // Shared vertex input: pos(vec3) + size(float) + alpha(float) = 5 floats
    VkVertexInputBindingDescription binding = tightVertexBinding(5 * sizeof(float));
    std::vector<VkVertexInputAttributeDescription> attrs = positionPlusTwoFloatsAttrs();


    // ---- Rebuild ripple pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_ripple.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_ripple.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildRipplePipeline(device, vertStage, fragStage, binding, attrs);

    }

    // ---- Rebuild bubble pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_bubble.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_bubble.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildBubblePipeline(device, vertStage, fragStage, binding, attrs);

    }

    // ---- Rebuild insect pipeline ----
    {
        VkShaderModule vertModule;
        (void)vertModule.loadFromFile(device, "assets/shaders/swim_ripple.vert.spv");
        VkShaderModule fragModule;
        (void)fragModule.loadFromFile(device, "assets/shaders/swim_insect.frag.spv");

        VkPipelineShaderStageCreateInfo vertStage = vertModule.stageInfo(VK_SHADER_STAGE_VERTEX_BIT);
        VkPipelineShaderStageCreateInfo fragStage = fragModule.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT);

        buildInsectPipeline(device, vertStage, fragStage, binding, attrs);

    }
    if (surfaceRippleLayout_) createSurfaceRipplePipeline();
}

void SwimEffects::spawnFootSplash(const glm::vec3& footPos, float waterH) {
    // Small burst of splash droplets at foot position (for wading)
    constexpr int splashCount = 5;
    for (int i = 0; i < splashCount; ++i) {
        if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) break;
        Particle p;
        float ox = randFloat(-0.4f, 0.4f);
        float oy = randFloat(-0.4f, 0.4f);
        p.position = glm::vec3(footPos.x + ox, footPos.y + oy, waterH + 0.1f);
        // Small upward spray in random horizontal direction
        float angle = randFloat(0.0f, 6.2832f);
        float speed = randFloat(0.8f, 2.0f);
        p.velocity = glm::vec3(std::cos(angle) * speed, std::sin(angle) * speed, randFloat(1.0f, 2.5f));
        p.lifetime = 0.0f;
        p.maxLifetime = randFloat(0.3f, 0.6f);
        p.size = randFloat(2.0f, 4.0f);
        p.alpha = randFloat(0.4f, 0.7f);
        ripples.push_back(p);
    }
}

void SwimEffects::spawnBubble(const glm::vec3& pos, float /*waterH*/) {
    if (static_cast<int>(bubbles.size()) >= MAX_BUBBLE_PARTICLES) return;

    Particle p;
    float ox = randFloat(-3.0f, 3.0f);
    float oy = randFloat(-3.0f, 3.0f);
    float oz = randFloat(-2.0f, 0.0f);
    p.position = glm::vec3(pos.x + ox, pos.y + oy, pos.z + oz);

    p.velocity = glm::vec3(randFloat(-0.3f, 0.3f), randFloat(-0.3f, 0.3f), randFloat(4.0f, 8.0f));
    p.lifetime = 0.0f;
    p.maxLifetime = randFloat(2.0f, 3.5f);
    p.size = randFloat(6.0f, 12.0f);
    p.alpha = 0.6f;

    bubbles.push_back(p);
}

void SwimEffects::spawnInsect(const glm::vec3& vegPos) {
    if (static_cast<int>(insects.size()) >= MAX_INSECT_PARTICLES) return;

    InsectParticle p;
    p.orbitCenter = vegPos;
    p.phase = randFloat(0.0f, 6.2832f);
    p.orbitRadius = randFloat(0.5f, 2.0f);
    p.orbitSpeed = randFloat(1.5f, 4.0f);
    p.heightOffset = randFloat(0.5f, 3.0f);
    p.lifetime = 0.0f;
    p.maxLifetime = randFloat(3.0f, 8.0f);
    p.size = randFloat(2.0f, 3.0f);
    p.alpha = randFloat(0.6f, 0.9f);

    // Start at orbit position
    float angle = p.phase;
    p.position = vegPos + glm::vec3(
        std::cos(angle) * p.orbitRadius,
        std::sin(angle) * p.orbitRadius,
        p.heightOffset
    );

    insects.push_back(p);
}

void SwimEffects::update(const Camera& camera, const CameraController& cc,
                         const WaterRenderer& water, float deltaTime) {
    glm::vec3 camPos = camera.getPosition();

    // Use character position for ripples in third-person mode
    glm::vec3 charPos = camPos;
    const glm::vec3* followTarget = cc.getFollowTarget();
    if (cc.isThirdPerson() && followTarget) {
        charPos = *followTarget;
    }

    // Check water at character position (for ripples) and camera position (for bubbles).
    // Ripples, spray and bubbles are water's: in lava or slime (a magma
    // material) the client throws none of them, and blue rings and rising
    // bubbles read as swimming in water. Decided as each is made, not reset
    // after: GCC 16 loses track of a reset optional and warns it may be read
    // uninitialised.
    const bool charInMagma =
        water.getLiquidKindAt(charPos.x, charPos.y) == client_liquid::MaterialKind::Magma;
    const bool camInMagma =
        water.getLiquidKindAt(camPos.x, camPos.y) == client_liquid::MaterialKind::Magma;
    const std::optional<float> charWaterH =
        charInMagma ? std::nullopt : water.getWaterHeightAt(charPos.x, charPos.y);
    const std::optional<float> camWaterH =
        camInMagma ? std::nullopt : water.getWaterHeightAt(camPos.x, camPos.y);

    bool swimming = cc.isSwimming();
    bool moving = cc.isMoving();

    updateSurfaceRipples(cc, charPos, charWaterH, deltaTime);

    // The foot splashes below live on in `ripples`, and go out with their lifetime.

    // Feet in the water but not swimming: the shoreline transition. Ripples
    // above only spawn while swimming, so walking or riding through the
    // shallows threw nothing but the per-footstep burst, which reads as dry.
    // Rate rises with depth, since deeper water is displaced harder, and dies
    // off as the surface approaches the character's waist, by which point
    // swimming takes over.
    if (!swimming && moving && charWaterH) {
        const float depth = *charWaterH - charPos.z;
        constexpr float kWadeMinDepth = 0.03f;
        constexpr float kWadeMaxDepth = 1.60f;
        if (depth > kWadeMinDepth && depth < kWadeMaxDepth) {
            const float depthScale = glm::clamp(depth / 0.7f, 0.30f, 1.0f);
            // Its own accumulator: the swim branch above zeroes rippleSpawnAccum
            // on every frame it is not swimming, so a wading rate of 30/s could
            // only ever reach 0.5 in a frame and never crossed the threshold.
            wadeSpawnAccum += 30.0f * depthScale * deltaTime;

            // Forward from yaw is (cos, sin) - the same derivation the camera
            // controller uses to move the character.
            const float yawRad = glm::radians(cc.getYaw());
            const glm::vec2 travel(std::cos(yawRad), std::sin(yawRad));

            while (wadeSpawnAccum >= 1.0f) {
                wadeSpawnAccum -= 1.0f;
                if (static_cast<int>(ripples.size()) >= MAX_RIPPLE_PARTICLES) break;
                Particle p;
                // Around the legs, biased behind so the spray trails the stride.
                const float side = randFloat(-0.45f, 0.45f);
                const float back = randFloat(-0.55f, 0.15f);
                p.position = glm::vec3(charPos.x + travel.x * back - travel.y * side,
                                       charPos.y + travel.y * back + travel.x * side,
                                       *charWaterH + 0.05f);
                const float angle = randFloat(0.0f, 6.2832f);
                const float outward = randFloat(0.5f, 1.6f) * depthScale;
                p.velocity = glm::vec3(std::cos(angle) * outward - travel.x * 0.8f,
                                       std::sin(angle) * outward - travel.y * 0.8f,
                                       randFloat(0.9f, 2.2f) * depthScale);
                p.lifetime = 0.0f;
                p.maxLifetime = randFloat(0.25f, 0.55f);
                p.size = randFloat(1.6f, 3.4f) * depthScale;
                p.alpha = randFloat(0.35f, 0.65f) * depthScale;
                ripples.push_back(p);
            }
        } else {
            wadeSpawnAccum = 0.0f;
        }
    } else {
        wadeSpawnAccum = 0.0f;
    }

    // --- Bubble spawning ---
    // Require swimming state to prevent spurious bubbles on login/teleport
    // when camera may briefly appear below a water surface before grounding.
    bool underwater = swimming && camWaterH && camPos.z < *camWaterH;
    if (underwater) {
        float bubbleRate = 20.0f;
        bubbleSpawnAccum += bubbleRate * deltaTime;
        while (bubbleSpawnAccum >= 1.0f) {
            spawnBubble(camPos, *camWaterH);
            bubbleSpawnAccum -= 1.0f;
        }
    } else {
        bubbleSpawnAccum = 0.0f;
        bubbles.clear();
    }

    // --- Insect spawning near water vegetation ---
    if (m2Renderer) {
        auto vegPositions = m2Renderer->getWaterVegetationPositions(camPos, 60.0f);
        if (!vegPositions.empty()) {
            // Spawn rate: ~4/sec per nearby vegetation cluster (capped by MAX_INSECT_PARTICLES)
            float spawnRate = std::min(static_cast<float>(vegPositions.size()) * 4.0f, 20.0f);
            insectSpawnAccum += spawnRate * deltaTime;
            while (insectSpawnAccum >= 1.0f && static_cast<int>(insects.size()) < MAX_INSECT_PARTICLES) {
                // Pick a random vegetation position to spawn near
                int idx = static_cast<int>(randFloat(0.0f, static_cast<float>(vegPositions.size()) - 0.01f));
                spawnInsect(vegPositions[idx]);
                insectSpawnAccum -= 1.0f;
            }
            if (insectSpawnAccum > 2.0f) insectSpawnAccum = 0.0f;
        }
    }

    // --- Update ripples (splash droplets with gravity) ---
    for (int i = static_cast<int>(ripples.size()) - 1; i >= 0; --i) {
        auto& p = ripples[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime) {
            ripples[i] = ripples.back();
            ripples.pop_back();
            continue;
        }
        // Apply gravity to splash droplets
        p.velocity.z -= 9.8f * deltaTime;
        p.position += p.velocity * deltaTime;

        // Kill if fallen back below water
        float surfaceZ = charWaterH ? *charWaterH : 0.0f;
        if (p.position.z < surfaceZ && p.lifetime > 0.1f) {
            ripples[i] = ripples.back();
            ripples.pop_back();
            continue;
        }

        float t = p.lifetime / p.maxLifetime;
        p.alpha = glm::mix(0.7f, 0.0f, t);
        p.size = glm::mix(5.0f, 2.0f, t);
    }

    // --- Update bubbles ---
    float bubbleCeilH = camWaterH ? *camWaterH : 0.0f;
    for (int i = static_cast<int>(bubbles.size()) - 1; i >= 0; --i) {
        auto& p = bubbles[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime || p.position.z >= bubbleCeilH) {
            bubbles[i] = bubbles.back();
            bubbles.pop_back();
            continue;
        }
        // Wobble
        float wobbleX = std::sin(p.lifetime * 3.0f) * 0.5f;
        float wobbleY = std::cos(p.lifetime * 2.5f) * 0.5f;
        p.position += (p.velocity + glm::vec3(wobbleX, wobbleY, 0.0f)) * deltaTime;

        float t = p.lifetime / p.maxLifetime;
        if (t > 0.8f) {
            p.alpha = 0.6f * (1.0f - (t - 0.8f) / 0.2f);
        } else {
            p.alpha = 0.6f;
        }
    }

    // --- Update insects (erratic orbiting flight) ---
    for (int i = static_cast<int>(insects.size()) - 1; i >= 0; --i) {
        auto& p = insects[i];
        p.lifetime += deltaTime;
        if (p.lifetime >= p.maxLifetime) {
            insects[i] = insects.back();
            insects.pop_back();
            continue;
        }

        float t = p.lifetime / p.maxLifetime;
        float time = p.lifetime * p.orbitSpeed + p.phase;

        // Erratic looping: primary orbit + secondary wobble
        float primaryAngle = time;
        float wobbleAngle = std::sin(time * 2.3f) * 0.8f;
        float radius = p.orbitRadius + std::sin(time * 1.7f) * 0.3f;

        float heightWobble = std::sin(time * 1.1f + p.phase * 0.5f) * 0.5f;

        p.position = p.orbitCenter + glm::vec3(
            std::cos(primaryAngle + wobbleAngle) * radius,
            std::sin(primaryAngle + wobbleAngle) * radius,
            p.heightOffset + heightWobble
        );

        // Fade in/out
        if (t < 0.1f) {
            p.alpha = glm::mix(0.0f, 0.8f, t / 0.1f);
        } else if (t > 0.85f) {
            p.alpha = glm::mix(0.8f, 0.0f, (t - 0.85f) / 0.15f);
        } else {
            p.alpha = 0.8f;
        }
    }

    // --- Build vertex data ---
    rippleVertexData.clear();
    rippleVertexData.reserve(ripples.size() * 5);
    for (const auto& p : ripples) {
        rippleVertexData.push_back(p.position.x);
        rippleVertexData.push_back(p.position.y);
        rippleVertexData.push_back(p.position.z);
        rippleVertexData.push_back(p.size);
        rippleVertexData.push_back(p.alpha);
    }

    bubbleVertexData.clear();
    bubbleVertexData.reserve(bubbles.size() * 5);
    for (const auto& p : bubbles) {
        bubbleVertexData.push_back(p.position.x);
        bubbleVertexData.push_back(p.position.y);
        bubbleVertexData.push_back(p.position.z);
        bubbleVertexData.push_back(p.size);
        bubbleVertexData.push_back(p.alpha);
    }

    insectVertexData.clear();
    insectVertexData.reserve(insects.size() * 5);
    for (const auto& p : insects) {
        insectVertexData.push_back(p.position.x);
        insectVertexData.push_back(p.position.y);
        insectVertexData.push_back(p.position.z);
        insectVertexData.push_back(p.size);
        insectVertexData.push_back(p.alpha);
    }
}

void SwimEffects::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    renderSurfaceRipples(cmd, perFrameSet);
    if (rippleVertexData.empty() && bubbleVertexData.empty() && insectVertexData.empty()) return;

    VkDeviceSize offset = 0;

    // --- Render ripples (splash droplets above water surface) ---
    if (!rippleVertexData.empty() && ripplePipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = rippleVertexData.size() * sizeof(float);
        if (rippleDynamicVBAllocInfo.pMappedData) {
            std::memcpy(rippleDynamicVBAllocInfo.pMappedData, rippleVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ripplePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ripplePipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &rippleDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(rippleVertexData.size() / 5), 1, 0, 0);
    }

    // --- Render bubbles ---
    if (!bubbleVertexData.empty() && bubblePipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = bubbleVertexData.size() * sizeof(float);
        if (bubbleDynamicVBAllocInfo.pMappedData) {
            std::memcpy(bubbleDynamicVBAllocInfo.pMappedData, bubbleVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bubblePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bubblePipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &bubbleDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(bubbleVertexData.size() / 5), 1, 0, 0);
    }

    // --- Render insects ---
    if (!insectVertexData.empty() && insectPipeline != VK_NULL_HANDLE) {
        VkDeviceSize uploadSize = insectVertexData.size() * sizeof(float);
        if (insectDynamicVBAllocInfo.pMappedData) {
            std::memcpy(insectDynamicVBAllocInfo.pMappedData, insectVertexData.data(), uploadSize);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, insectPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, insectPipelineLayout,
            0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &insectDynamicVB, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(insectVertexData.size() / 5), 1, 0, 0);
    }
}


namespace {

// What the client's ripple draws with: the footprints' quad and vertex stage.
struct SurfaceRipplePush {
    glm::mat4 model{1.0f};
    glm::vec4 tint{1.0f};
};

// The unit's virtual at 0x3c, which sizes its ripples. Not identified: 4 gives
// the ring the width it has around a character in the original's shot.
constexpr float kSurfaceRippleSizeValue = 4.0f;

float rippleRoll() {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

}  // namespace

bool SwimEffects::createSurfaceRipplePipeline() {
    VkDevice device = vkCtx->getDevice();
    VkShaderModule vert;
    VkShaderModule frag;
    if (!vert.loadFromFile(device, "assets/shaders/footprint.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/water_ripple.frag.spv")) {
        vert.destroy();
        frag.destroy();
        return false;
    }
    VkVertexInputBindingDescription binding{.binding = 0, .stride = 5 * sizeof(float),
                                            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription pos{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
                                          .offset = 0};
    VkVertexInputAttributeDescription uv{.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
                                         .offset = 3 * sizeof(float)};
    surfaceRipplePipeline_ = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({binding}, {pos, uv})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, depth::kNearerOrEqual)
        .setColorBlendAttachment(PipelineBuilder::blendAdditive())
        .setMultisample(targetSamples_)
        .setLayout(surfaceRippleLayout_)
        .setRenderPass(targetPass_)
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
    vert.destroy();
    frag.destroy();
    return surfaceRipplePipeline_ != VK_NULL_HANDLE;
}

void SwimEffects::loadRippleTextures(pipeline::AssetManager* assets) {
    if (!vkCtx || !assets || surfaceRippleLayout_) return;
    VkDevice device = vkCtx->getDevice();
    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    surfaceRippleSetLayout_ = createDescriptorSetLayout(device, {sampler});
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 2;
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 2;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!surfaceRippleSetLayout_ ||
        vkCreateDescriptorPool(device, &pool, nullptr, &surfaceRipplePool_) != VK_SUCCESS) return;
    const std::array<VkDescriptorSetLayout, 2> layouts{surfaceRippleSetLayout_, surfaceRippleSetLayout_};
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = surfaceRipplePool_;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &alloc, surfaceRippleSets_.data()) != VK_SUCCESS) return;

    const std::array<const char*, 2> paths{"XTextures\\splash\\splash.blp", "XTextures\\splash\\wake.blp"};
    for (size_t i = 0; i < paths.size(); ++i) {
        pipeline::BLPImage image = assets->loadTexture(paths[i]);
        if (!image.isValid() || !surfaceRippleTextures_[i].upload(*vkCtx, image.data.data(), image.width,
                                                                  image.height, VK_FORMAT_R8G8B8A8_UNORM, true)) {
            LOG_WARNING("SwimEffects: failed to load ", paths[i]);
            return;
        }
        surfaceRippleTextures_[i].createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (!surfaceRippleTextures_[i].isValid()) return;
        VkDescriptorImageInfo imageInfo = surfaceRippleTextures_[i].descriptorInfo();
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = surfaceRippleSets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    const float vertices[] = {
        -0.5f, -0.5f, 0.0f, 0.0f, 1.0f,
         0.5f, -0.5f, 0.0f, 1.0f, 1.0f,
        -0.5f,  0.5f, 0.0f, 0.0f, 0.0f,
         0.5f,  0.5f, 0.0f, 1.0f, 0.0f
    };
    AllocatedBuffer buffer = uploadBuffer(*vkCtx, vertices, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    surfaceRippleQuad_ = buffer.buffer;
    surfaceRippleQuadAlloc_ = buffer.allocation;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(SurfaceRipplePush);
    surfaceRippleLayout_ = createPipelineLayout(device, {perFrameLayout_, surfaceRippleSetLayout_}, {push});
    if (surfaceRippleLayout_ && !createSurfaceRipplePipeline()) {
        LOG_WARNING("SwimEffects: no pipeline for the ripples");
    }
}

void SwimEffects::updateSurfaceRipples(const CameraController& cc, const glm::vec3& charPos,
                                       std::optional<float> waterH, float deltaTime) {
    namespace wr = water_ripple;
    std::erase_if(surfaceRipples_, [&](wr::Ripple& r) { return !wr::step(r, deltaTime); });
    surfaceRippleDelay_ -= deltaTime;
    if (!waterH || !surfaceRipplesShown_ || *waterH <= charPos.z) {
        surfaceRippleLastDepth_ = 0.0f;
        return;
    }
    wr::Unit u;
    u.surfacePoint = glm::vec3(charPos.x, charPos.y, *waterH + 0.03f);
    u.depth = *waterH - charPos.z;
    u.height = cc.collisionHeight();
    u.speed = cc.isSwimming() ? 4.722222f : 7.0f;
    u.facing = glm::radians(cc.getFacingYaw());
    u.sizeValue = kSurfaceRippleSizeValue;
    if (cc.isMovingForward()) u.moveFlags |= wr::kForward;
    if (cc.isMovingBackward()) u.moveFlags |= wr::kBackward;
    if (cc.isStrafingLeft()) u.moveFlags |= wr::kStrafeLeft;
    if (cc.isStrafingRight()) u.moveFlags |= wr::kStrafeRight;
    u.mode = u.moveFlags != 0 ? wr::Mode::Moving
             : cc.isTurningLeft() || cc.isTurningRight() ? wr::Mode::Turning
                                                          : wr::Mode::Still;
    // Across four tenths of the height, at once (0x00730d10 passes it 0xc9).
    const float mark = u.height * 0.4f;
    if ((mark < u.depth) != (mark < surfaceRippleLastDepth_)) {
        u.mode = wr::Mode::Crossing;
        surfaceRippleDelay_ = 0.0f;
    }
    surfaceRippleLastDepth_ = u.depth;
    if (surfaceRippleDelay_ > 0.0f) return;
    const wr::Rolls rolls{rippleRoll(), rippleRoll(), rippleRoll(), rippleRoll(), rippleRoll()};
    wr::Ripple ripple;
    float next = 0.0f;
    if (wr::spawn(u, rolls, ripple, next) && surfaceRipples_.size() < 96) {
        surfaceRipples_.push_back(ripple);
        surfaceRippleDelay_ = next;
    }
}

void SwimEffects::renderSurfaceRipples(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (surfaceRipples_.empty() || !surfaceRipplesShown_ || !surfaceRipplePipeline_ || !surfaceRippleQuad_) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, surfaceRipplePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, surfaceRippleLayout_, 0, 1, &perFrameSet, 0,
                            nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &surfaceRippleQuad_, &offset);
    for (const water_ripple::Ripple& r : surfaceRipples_) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), r.position);
        model = glm::rotate(model, r.angle, glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, glm::vec3(r.size * 2.0f, r.size * 2.0f, 1.0f));
        SurfaceRipplePush push;
        push.model = model;
        push.tint.a = std::clamp(r.alpha, 0.0f, 1.0f);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, surfaceRippleLayout_, 1, 1,
                                &surfaceRippleSets_[r.wake ? 1 : 0], 0, nullptr);
        vkCmdPushConstants(cmd, surfaceRippleLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
}

} // namespace rendering
} // namespace wowee
