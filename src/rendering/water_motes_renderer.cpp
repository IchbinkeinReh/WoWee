#include "rendering/water_motes_renderer.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/depth_convention.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee {
namespace rendering {

namespace wm = water_motes;

namespace {

struct Vertex {
    float position[3];
    float uv[2];
};
static_assert(sizeof(Vertex) == 20);

struct Push {
    glm::vec4 params;   // x: fogged
};

constexpr uint32_t kMostVertices = static_cast<uint32_t>(wm::kMostDrawn) * 6;

}  // namespace

bool WaterMotesRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    VkDevice device = vkCtx_->getDevice();
    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureLayout_ = createDescriptorSetLayout(device, {sampler});
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!textureLayout_ || vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) {
        LOG_ERROR("Water motes: failed to create descriptor resources");
        return false;
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(Push);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, textureLayout_}, {push});
    if (!pipelineLayout_ || !buildPipeline()) {
        LOG_ERROR("Water motes: failed to create the pipeline");
        return false;
    }
    for (int i = 0; i < kFrames; ++i) {
        AllocatedBuffer buf = createBuffer(vkCtx_->getAllocator(), kMostVertices * sizeof(Vertex),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        buffer_[i] = buf.buffer;
        allocation_[i] = buf.allocation;
        mapped_[i] = buf.info.pMappedData;
    }
    return true;
}

bool WaterMotesRenderer::buildPipeline() {
    VkDevice device = vkCtx_->getDevice();
    auto shaders = loadShaderPair(device, "assets/shaders/water_motes.vert.spv", "assets/shaders/water_motes.frag.spv",
                                  "water motes");
    if (!shaders) return false;
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attrs = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
    };
    // Alpha blended, the depth asked and not written (0x0079ca70).
    pipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({binding}, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, depth::kNearerOrEqual)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    return pipeline_ != VK_NULL_HANDLE;
}

void WaterMotesRenderer::recreatePipelines() {
    if (!vkCtx_ || !pipelineLayout_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    if (!buildPipeline()) LOG_ERROR("Water motes: failed to create the pipeline");
}

void WaterMotesRenderer::loadAssets(pipeline::AssetManager* assets) {
    if (!assets || !assets->isInitialized() || assetsLoaded_ || !vkCtx_ || !descriptorPool_) return;
    assetsLoaded_ = true;
    auto dbc = assets->loadDBC("LiquidType.dbc");
    // 3.x has the columns; the layouts before have none of them.
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 45) return;
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        wm::Liquid l;
        l.flags = dbc->getUInt32(r, 2);
        l.scale = dbc->getFloat(r, 11);
        l.movement = dbc->getUInt32(r, 12);
        l.slots = dbc->getUInt32(r, 13);
        liquids_[dbc->getUInt32(r, 0)] = l;
    }
    pipeline::BLPImage image = assets->loadTexture("Textures\\WaterPoop02.blp");
    VkDevice device = vkCtx_->getDevice();
    if (!image.isValid() ||
        !texture_.upload(*vkCtx_, image.data.data(), image.width, image.height, VK_FORMAT_R8G8B8A8_UNORM, true) ||
        !texture_.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)) {
        LOG_WARNING("Water motes: failed to load Textures\\WaterPoop02.blp");
        return;
    }
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptorPool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &textureLayout_;
    if (vkAllocateDescriptorSets(device, &alloc, &textureSet_) != VK_SUCCESS) {
        textureSet_ = VK_NULL_HANDLE;
        return;
    }
    VkDescriptorImageInfo info = texture_.descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = textureSet_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void WaterMotesRenderer::update(float deltaTime, const Camera& camera, uint32_t liquidType) {
    vertices_ = 0;
    if (!textureSet_ || !pipeline_) return;
    if (liquidType != liquidType_) {
        liquidType_ = liquidType;
        const auto it = liquids_.find(liquidType);
        liquid_ = it != liquids_.end() ? it->second : wm::Liquid{};
        if (liquid_.motes()) swarm_.scatter(liquid_.scale);
    }
    if (liquidType == 0 || !liquid_.motes()) return;
    const glm::vec3 eye = camera.getPosition();
    swarm_.update(deltaTime, eye, liquid_);

    frame_ = (frame_ + 1) % kFrames;
    auto* out = static_cast<Vertex*>(mapped_[frame_]);
    if (!out) return;
    const glm::vec3 right = camera.getRight(), up = camera.getUp(), ahead = camera.getForward();
    size_t drawn = 0;
    const auto& motes = swarm_.motes();
    for (size_t i = 0; i < motes.size() && drawn < wm::kMostDrawn; ++i) {
        const wm::Mote& m = motes[i];
        if (!wm::Swarm::seen({glm::dot(m.at, right), glm::dot(m.at, up), glm::dot(m.at, ahead)})) continue;
        ++drawn;
        const glm::vec3 at = eye + m.at;
        const glm::vec3 x = right * (m.size * 0.5f), y = up * (m.size * 0.5f);
        const glm::vec2 cell = wm::cell(liquid_.slots, i);
        const glm::vec3 corner[4] = {at - x + y, at - x - y, at + x + y, at + x - y};
        const glm::vec2 uv[4] = {cell, cell + glm::vec2(0.0f, wm::kCellSide), cell + glm::vec2(wm::kCellSide, 0.0f),
                                 cell + glm::vec2(wm::kCellSide)};
        for (const int k : {0, 1, 2, 2, 1, 3})
            out[vertices_++] = {{corner[k].x, corner[k].y, corner[k].z}, {uv[k].x, uv[k].y}};
    }
}

void WaterMotesRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (vertices_ == 0 || !pipeline_ || !textureSet_) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkDescriptorSet sets[2] = {perFrameSet, textureSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
    const Push push{{liquid_.fogged() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer_[frame_], &offset);
    vkCmdDraw(cmd, vertices_, 1, 0, 0);
}

void WaterMotesRenderer::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    ::wowee::rendering::deviceWaitIdle(device);
    texture_.destroy(device, allocator);
    for (int i = 0; i < kFrames; ++i) destroy(allocator, buffer_[i], allocation_[i]);
    destroy(device, pipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, descriptorPool_);
    destroy(device, textureLayout_);
    textureSet_ = VK_NULL_HANDLE;
    vkCtx_ = nullptr;
}

}  // namespace rendering
}  // namespace wowee
