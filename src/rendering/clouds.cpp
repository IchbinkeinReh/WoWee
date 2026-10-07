#include "rendering/clouds.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/day_night.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstddef>

namespace wowee {
namespace rendering {

Clouds::Clouds() = default;

Clouds::~Clouds() {
    shutdown();
}

/// Builds the one pipeline this effect draws with.
///
/// initialize() and recreatePipelines() both need it, described identically,
/// and the vertex layout went with it: a stride and an attribute stated twice
/// in one file are two chances to disagree about what a vertex is.
void Clouds::buildPipeline(VkDevice device,
                           const VkPipelineShaderStageCreateInfo& vertStage,
                           const VkPipelineShaderStageCreateInfo& fragStage) {
    // ------------------------------------------------------------------ vertex input
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(CloudVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription posAttr{};
    posAttr.location = 0;
    posAttr.binding  = 0;
    posAttr.format   = VK_FORMAT_R32G32B32_SFLOAT;
    posAttr.offset   = offsetof(CloudVertex, pos);

    VkVertexInputAttributeDescription uvAttr{};
    uvAttr.location = 1;
    uvAttr.binding  = 0;
    uvAttr.format   = VK_FORMAT_R32G32B32_SFLOAT;
    uvAttr.offset   = offsetof(CloudVertex, uvAlpha);

    std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    // ------------------------------------------------------------------ pipeline
    pipeline_ = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, {posAttr, uvAttr})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx_->getPipelineCache());


}

bool Clouds::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing cloud system (Vulkan)");

    vkCtx_ = ctx;
    VkDevice device = vkCtx_->getDevice();

    // ------------------------------------------------------------------ shaders
    auto shaders = loadShaderPair(device, "assets/shaders/clouds.vert.spv", "assets/shaders/clouds.frag.spv", "clouds");
    if (!shaders) return false;
    const auto& vertStage = shaders.vertStage;
    const auto& fragStage = shaders.fragStage;

    // ------------------------------------------------------------------ push constants
    // Fragment-only push: 4 x vec4 = 64 bytes
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(CloudPush); // 64 bytes

    // ------------------------------------------------------------------ pipeline layout
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout}, {pushRange});
    if (pipelineLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create clouds pipeline layout");
        return false;
    }

    buildPipeline(device, vertStage, fragStage);

    if (pipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create clouds pipeline");
        return false;
    }

    // ------------------------------------------------------------------ geometry
    generateMesh();
    createBuffers();

    LOG_INFO("Cloud system initialized: ", indexCount_ / 3, " triangles");
    return true;
}

void Clouds::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    destroy(device, pipeline_);

    auto shaders = loadShaderPair(device, "assets/shaders/clouds.vert.spv", "assets/shaders/clouds.frag.spv", "clouds");
    if (!shaders) return;
    const auto& vertStage = shaders.vertStage;
    const auto& fragStage = shaders.fragStage;

    buildPipeline(device, vertStage, fragStage);
}

void Clouds::shutdown() {
    destroyBuffers();

    if (vkCtx_) destroyPipeline(vkCtx_->getDevice(), pipeline_, pipelineLayout_);

    vkCtx_ = nullptr;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void Clouds::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const SkyParams& params) {
    if (!enabled_ || pipeline_ == VK_NULL_HANDLE) {
        return;
    }

    // The client's cloud texture inputs (0x007efae0, 0x007efd00): the three
    // cloud colours, the cover threshold from float band 3, and the sun -
    // from 04:50 to 22:10 - or else the moon, placed over the texture where
    // its direction meets the dome, at 64 texels' height (flatter in
    // weather) and glowing less in it.
    const float dayFraction = params.timeOfDay / 24.0f;
    const float storm = daynight::stormBlend(params.weatherIntensity);
    glm::vec3 lightDir = daynight::cloudsLitBySun(dayFraction) ? params.sunDir : params.moonDir;
    const float lenSq = glm::dot(lightDir, lightDir);
    lightDir = lenSq > 1e-8f ? lightDir * glm::inversesqrt(lenSq) : glm::vec3(0.0f, 0.0f, 1.0f);
    const float radius = daynight::cloudTextureRadius(daynight::skyDomePolarFraction(lightDir));
    glm::vec2 across(lightDir.x, lightDir.y);
    const float acrossLen = glm::length(across);
    across = acrossLen > 1e-6f ? across / acrossLen : glm::vec2(0.0f);
    const glm::vec2 lightTexel = (glm::vec2(0.5f) + across * radius) * 128.0f;

    density_ = glm::clamp(params.cloudDensity, 0.0f, 1.0f);

    CloudPush push{};
    push.sunLit = glm::vec4(params.cloudSunColor, daynight::cloudGlow(storm));
    push.shade  = glm::vec4(params.cloudShadeColor, daynight::cloudCoverageThreshold(density_));
    push.base   = glm::vec4(params.cloudBaseColor, daynight::cloudLightHeight(storm));
    push.light  = glm::vec4(lightTexel, noiseTime_, 0.0f);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
        0, 1, &perFrameSet, 0, nullptr);

    vkCmdPushConstants(cmd, pipelineLayout_,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indexCount_), 1, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void Clouds::update(float deltaTime) {
    if (!enabled_) {
        return;
    }
    // The noise's third axis, in lattice cells: the clouds change shape in
    // place over a minute or two. The client's rate (the object's +0xc) is
    // set at runtime and was not traced.
    noiseTime_ += deltaTime * 0.01f;
    if (noiseTime_ > 1000.0f) noiseTime_ -= 1000.0f;
}

// ---------------------------------------------------------------------------
// The client's cloud dome (0x007f20e0)
// ---------------------------------------------------------------------------

void Clouds::generateMesh() {
    vertices_.clear();
    indices_.clear();

    // The rows on the sky sphere, which is centred cos(45) below the eye, so
    // a vertex is its own direction from the eye. The texture is laid flat:
    // row i is i/11 of the way to its edge.
    constexpr float kCentreDepth = 0.70710678f;
    constexpr int kRows = 12;
    for (int ring = 0; ring < kRows; ++ring) {
        const float phi = daynight::kCloudDomeRows[ring] * static_cast<float>(M_PI);
        const float uvRadius = 0.5f * static_cast<float>(ring) / static_cast<float>(kRows - 1);
        const float alpha = static_cast<float>(daynight::kCloudDomeAlpha[ring]) / 255.0f;
        for (int seg = 0; seg <= SEGMENTS; ++seg) {
            const float theta = (static_cast<float>(seg) / SEGMENTS) * (2.0f * static_cast<float>(M_PI));
            const float sx = std::sin(theta);
            const float cy = std::cos(theta);
            CloudVertex v{};
            v.pos = glm::vec3(sx * std::sin(phi), cy * std::sin(phi), std::cos(phi) - kCentreDepth);
            v.uvAlpha = glm::vec3(0.5f + sx * uvRadius, 0.5f + cy * uvRadius, alpha);
            vertices_.push_back(v);
        }
    }

    for (int ring = 0; ring < kRows - 1; ++ring) {
        for (int seg = 0; seg < SEGMENTS; ++seg) {
            uint32_t current = static_cast<uint32_t>(ring * (SEGMENTS + 1) + seg);
            uint32_t next    = current + static_cast<uint32_t>(SEGMENTS + 1);

            indices_.push_back(current);
            indices_.push_back(next);
            indices_.push_back(current + 1);

            indices_.push_back(current + 1);
            indices_.push_back(next);
            indices_.push_back(next + 1);
        }
    }

    indexCount_ = static_cast<int>(indices_.size());
}

// ---------------------------------------------------------------------------
// GPU buffer management
// ---------------------------------------------------------------------------

void Clouds::createBuffers() {
    AllocatedBuffer vbuf = uploadBuffer(*vkCtx_,
        vertices_.data(),
        vertices_.size() * sizeof(CloudVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    vertexBuffer_ = vbuf.buffer;
    vertexAlloc_  = vbuf.allocation;

    AllocatedBuffer ibuf = uploadBuffer(*vkCtx_,
        indices_.data(),
        indices_.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    indexBuffer_ = ibuf.buffer;
    indexAlloc_  = ibuf.allocation;

    // CPU data no longer needed
    vertices_.clear();
    vertices_.shrink_to_fit();
    indices_.clear();
    indices_.shrink_to_fit();
}

void Clouds::destroyBuffers() {
    if (!vkCtx_) return;

    VmaAllocator allocator = vkCtx_->getAllocator();

    destroy(allocator, vertexBuffer_, vertexAlloc_);
    destroy(allocator, indexBuffer_, indexAlloc_);
}

} // namespace rendering
} // namespace wowee
