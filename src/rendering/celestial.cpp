#include "rendering/celestial.hpp"
#include "rendering/day_night.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee {
namespace rendering {

namespace {
// The textures 0x007f2790 (sprites) and 0x007ee150 / 0x007ee230 (glare) load.
constexpr const char* kTexturePaths[] = {
    "Textures\\sunCenter.blp", "Textures\\moon.blp", "Textures\\moon02.blp",
    "Textures\\sunGlare.blp", "Textures\\moonGlare.blp"};
}  // namespace

Celestial::Celestial() = default;

Celestial::~Celestial() {
    shutdown();
}

VkPipeline Celestial::buildPipeline(VkDevice device,
                                    const VkPipelineShaderStageCreateInfo& vertStage,
                                    const VkPipelineShaderStageCreateInfo& fragStage,
                                    bool glare) {
    // Vertex: vec3 pos + vec2 texCoord, stride = 20 bytes
    VkVertexInputBindingDescription binding = tightVertexBinding(5 * sizeof(float));
    std::vector<VkVertexInputAttributeDescription> attrs = positionPlusUvAttrs();
    std::vector<VkDynamicState> dynamicStates = viewportAndScissorDynamic();

    return PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        // The bodies: on the far plane and tested, never written - the ground
        // is drawn before the sky, and this keeps the sun behind a mountain;
        // alpha blended (0x009ac660 sets blend mode 2). The glare: drawn
        // after the world with no depth test at all and added on (0x009ac400
        // sets blend mode 3 and turns the depth test off); the occlusion
        // query fades it instead.
        .setDepthTest(!glare, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(glare ? PipelineBuilder::blendAdditive()
                                       : PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(dynamicStates)
        .build(device, vkCtx_->getPipelineCache());
}

bool Celestial::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    LOG_INFO("Initializing celestial renderer (Vulkan)");

    vkCtx_ = ctx;
    VkDevice device = vkCtx_->getDevice();

    auto shaders = loadShaderPair(device, "assets/shaders/celestial.vert.spv", "assets/shaders/celestial.frag.spv", "celestial");
    if (!shaders) return false;

    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    texSetLayout_ = createDescriptorSetLayout(device, {samplerBinding});
    if (texSetLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create celestial texture set layout");
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(CelestialPush);

    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout, texSetLayout_}, {pushRange});
    if (pipelineLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create celestial pipeline layout");
        return false;
    }

    pipeline_ = buildPipeline(device, shaders.vertStage, shaders.fragStage, false);
    glarePipeline_ = buildPipeline(device, shaders.vertStage, shaders.fragStage, true);
    if (pipeline_ == VK_NULL_HANDLE || glarePipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create celestial pipeline");
        return false;
    }

    createQuad();

    LOG_INFO("Celestial renderer initialized");
    return true;
}

void Celestial::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    destroy(device, pipeline_);
    destroy(device, glarePipeline_);

    auto shaders = loadShaderPair(device, "assets/shaders/celestial.vert.spv", "assets/shaders/celestial.frag.spv", "celestial");
    if (!shaders) return;

    pipeline_ = buildPipeline(device, shaders.vertStage, shaders.fragStage, false);
    glarePipeline_ = buildPipeline(device, shaders.vertStage, shaders.fragStage, true);
    if (pipeline_ == VK_NULL_HANDLE || glarePipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("Celestial::recreatePipelines: failed to create pipeline");
    }
}

void Celestial::shutdown() {
    destroyQuad();
    destroyTextures();

    if (vkCtx_) {
        VkDevice device = vkCtx_->getDevice();
        destroy(device, glarePipeline_);
        destroyPipeline(device, pipeline_, pipelineLayout_);
        destroy(device, texSetLayout_);
    }

    vkCtx_ = nullptr;
}

void Celestial::destroyTextures() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    for (int i = 0; i < TEX_COUNT; ++i) {
        textures_[i].destroy(device, vkCtx_->getAllocator());
        texSets_[i] = VK_NULL_HANDLE;
    }
    destroy(device, texPool_);
}

void Celestial::loadTextures(pipeline::AssetManager* assetManager) {
    if (!vkCtx_ || !assetManager || texSetLayout_ == VK_NULL_HANDLE) return;
    VkDevice device = vkCtx_->getDevice();
    vkDeviceWaitIdle(device);
    destroyTextures();

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = TEX_COUNT;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = TEX_COUNT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &texPool_) != VK_SUCCESS) {
        LOG_ERROR("Failed to create celestial descriptor pool");
        return;
    }

    for (int i = 0; i < TEX_COUNT; ++i) {
        pipeline::BLPImage blp = assetManager->loadTexture(kTexturePaths[i]);
        if (!blp.isValid()) {
            LOG_WARNING("Sky texture not found: ", kTexturePaths[i]);
            continue;
        }
        if (!textures_[i].upload(*vkCtx_, blp.data.data(), blp.width, blp.height,
                                 VK_FORMAT_R8G8B8A8_UNORM, true)) {
            LOG_WARNING("Failed to upload sky texture: ", kTexturePaths[i]);
            continue;
        }
        textures_[i].createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (!textures_[i].isValid()) continue;

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = texPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &texSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &allocInfo, &set) != VK_SUCCESS) continue;

        VkDescriptorImageInfo imgInfo = textures_[i].descriptorInfo();
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        texSets_[i] = set;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Celestial::drawSprite(VkCommandBuffer cmd, Tex tex, const glm::vec3& dir, float size,
                           const glm::vec4& color, bool body) {
    if (texSets_[tex] == VK_NULL_HANDLE || size <= 0.0f || color.a <= 0.0f) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            1, 1, &texSets_[tex], 0, nullptr);
    CelestialPush push{};
    push.dirSize = glm::vec4(dir, size);
    push.color = color;
    push.params = glm::vec4(body ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
}

void Celestial::bindQuad(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet perFrameSet) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
        0, 1, &perFrameSet, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
}

void Celestial::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Frame& frame) {
    if (!renderingEnabled_ || pipeline_ == VK_NULL_HANDLE) {
        return;
    }
    using namespace daynight;
    const float t = frame.dayFraction;
    bindQuad(cmd, pipeline_, perFrameSet);

    // The sun, the White Lady and the Blue Child, in that order (0x007f09b0
    // calls 0x009ac660 for each), each in ch9 at alpha 1 - storm (0x007f3230's
    // tail), on their own curves and at their own sizes (0x007eecc0). The
    // shaders cut each at the horizon and fade it in over the 0.4 units above
    // it as 0x007edee0 does.
    const glm::vec4 bodyColor(frame.color, 1.0f - frame.storm);
    drawSprite(cmd, TEX_SUN, frame.sunDir, sampleCurve(kSunSize, t), bodyColor, true);
    drawSprite(cmd, TEX_MOON, frame.moonDir, sampleCurve(kMoonSize, t) * kWhiteLadyScale,
               bodyColor, true);
    drawSprite(cmd, TEX_BLUE_CHILD, blueChildDirection(t),
               sampleCurve(kMoonSize, blueChildTime(t)) * kBlueChildScale, bodyColor, true);
}

void Celestial::updateGlare(const Frame& frame) {
    using namespace daynight;
    const float t = frame.dayFraction;

    // The glare (0x007ef6e0, for the sun and then the White Lady from
    // 0x007f0870): what it should show is the hour's curve, times whether
    // the body is above the horizon and not hidden, times how much sky the
    // sky models leave; what it does show steps toward that at its own rise
    // and fall rates. Its size and alpha grow from a facing of 0.7 to looking
    // straight at it.
    const auto step = [&](float& current, float target, const GlareDef& def) {
        if (target > current) current = std::min(target, current + def.riseRate * deltaTime_);
        else current = std::max(target, current - def.fallRate * deltaTime_);
    };
    const float skyLeft = 1.0f - std::clamp(frame.skyboxWeight, 0.0f, 1.0f);
    const float sunTarget = (frame.sunDir.z > 0.0f ? 1.0f : 0.0f) *
                            (1.0f - std::clamp(frame.sunOcclusion, 0.0f, 1.0f)) * skyLeft *
                            sampleCurve(kSunGlare.time, t);
    const float moonTarget = (frame.moonDir.z > 0.0f ? 1.0f : 0.0f) *
                             (1.0f - std::clamp(frame.moonOcclusion, 0.0f, 1.0f)) * skyLeft *
                             sampleCurve(kMoonGlare.time, t);
    step(sunGlare_, sunTarget, kSunGlare);
    step(moonGlare_, moonTarget, kMoonGlare);

    const float alpha = 1.0f - frame.storm;
    const float sunFacing = glareFacing(glm::dot(frame.cameraForward, frame.sunDir));
    glare_[0] = {frame.sunDir,
                 glm::mix(kSunGlare.sizeNear, kSunGlare.sizeFacing, sunFacing) * kSunGlare.sizeBase,
                 glm::vec4(frame.color, alpha *
                           glm::mix(kSunGlare.alphaNear, kSunGlare.alphaFacing, sunFacing) * sunGlare_)};
    // The moon's glare is the moon's own size, twice (0x007eecc0 writes the
    // size into both ends of its range).
    const float moonFacing = glareFacing(glm::dot(frame.cameraForward, frame.moonDir));
    glare_[1] = {frame.moonDir,
                 sampleCurve(kMoonSize, t) * kWhiteLadyScale * kMoonGlare.sizeBase,
                 glm::vec4(frame.color, alpha *
                           glm::mix(kMoonGlare.alphaNear, kMoonGlare.alphaFacing, moonFacing) * moonGlare_)};
    // How far the sun's glare darkens the world's light next frame
    // (0x007ef6e0 keeps it at 0xd38f4c, 0x007816f0 applies it).
    sunGlareDim_ = sunGlareWorldDim(glm::dot(frame.cameraForward, frame.sunDir), sunGlare_);
    glareReady_ = true;
}

void Celestial::renderGlare(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!renderingEnabled_ || glarePipeline_ == VK_NULL_HANDLE || !glareReady_) return;
    if (glare_[0].color.a <= 0.0f && glare_[1].color.a <= 0.0f) return;
    bindQuad(cmd, glarePipeline_, perFrameSet);
    drawSprite(cmd, TEX_SUN_GLARE, glare_[0].dir, glare_[0].size, glare_[0].color, false);
    drawSprite(cmd, TEX_MOON_GLARE, glare_[1].dir, glare_[1].size, glare_[1].color, false);
}

// ---------------------------------------------------------------------------
// GPU buffer management
// ---------------------------------------------------------------------------

void Celestial::createQuad() {
    // The client's quad, a unit across (0x007edbe0), centred on the body.
    float vertices[] = {
        // Position              TexCoord
        -0.5f,  0.5f, 0.0f,    0.0f, 0.0f, // Top-left
         0.5f,  0.5f, 0.0f,    1.0f, 0.0f, // Top-right
         0.5f, -0.5f, 0.0f,    1.0f, 1.0f, // Bottom-right
        -0.5f, -0.5f, 0.0f,    0.0f, 1.0f, // Bottom-left
    };

    uint32_t indices[] = { 0, 1, 2,  0, 2, 3 };

    AllocatedBuffer vbuf = uploadBuffer(*vkCtx_,
        vertices, sizeof(vertices),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    vertexBuffer_ = vbuf.buffer;
    vertexAlloc_  = vbuf.allocation;

    AllocatedBuffer ibuf = uploadBuffer(*vkCtx_,
        indices, sizeof(indices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    indexBuffer_ = ibuf.buffer;
    indexAlloc_  = ibuf.allocation;
}

void Celestial::destroyQuad() {
    if (!vkCtx_) return;

    VmaAllocator allocator = vkCtx_->getAllocator();

    destroy(allocator, vertexBuffer_, vertexAlloc_);
    destroy(allocator, indexBuffer_, indexAlloc_);
}

} // namespace rendering
} // namespace wowee
