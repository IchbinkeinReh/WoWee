#include "rendering/blob_shadow_renderer.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/frustum.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/wmo_renderer.hpp"

#include <cstring>

namespace wowee::rendering {

BlobShadowRenderer::~BlobShadowRenderer() {
    shutdown();
}

bool BlobShadowRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                    pipeline::AssetManager* assets) {
    if (!ctx || !assets) return false;
    if (vkCtx_) return true;
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    VkDevice device = vkCtx_->getDevice();

    // 0x007e4a40 loads it once, for every unit.
    pipeline::BLPImage image = assets->loadTexture("Textures\\ShadowBlob.blp");
    if (!image.isValid() ||
        !blobTexture_.upload(*vkCtx_, image.data.data(), image.width, image.height,
                             VK_FORMAT_R8G8B8A8_UNORM, true)) {
        LOG_WARNING("BlobShadowRenderer: Textures\\ShadowBlob.blp unavailable");
        shutdown();
        return false;
    }
    blobTexture_.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (!blobTexture_.isValid()) {
        shutdown();
        return false;
    }

    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureLayout_ = createDescriptorSetLayout(device, {sampler});
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!textureLayout_ || vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) {
        shutdown();
        return false;
    }
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptorPool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &textureLayout_;
    if (vkAllocateDescriptorSets(device, &alloc, &textureSet_) != VK_SUCCESS) {
        shutdown();
        return false;
    }
    VkDescriptorImageInfo imageInfo = blobTexture_.descriptorInfo();
    VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = textureSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(Push);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, textureLayout_}, {push});
    if (!pipelineLayout_ || !createPipeline()) {
        shutdown();
        return false;
    }

    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        AllocatedBuffer buf = createBuffer(vkCtx_->getAllocator(), kMaxVertices * sizeof(glm::vec3),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        vertexBuffer_[f] = buf.buffer;
        vertexAlloc_[f] = buf.allocation;
        vertexMapped_[f] = buf.info.pMappedData;
        if (!vertexBuffer_[f] || !vertexMapped_[f]) {
            shutdown();
            return false;
        }
    }
    LOG_INFO("BlobShadowRenderer: initialized");
    return true;
}

void BlobShadowRenderer::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkDeviceWaitIdle(device);
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
        destroy(allocator, vertexBuffer_[f], vertexAlloc_[f]);
        vertexMapped_[f] = nullptr;
    }
    blobTexture_.destroy(device, allocator);
    destroy(device, pipeline_);
    destroy(device, pipelineLayout_);
    destroy(device, descriptorPool_);
    destroy(device, textureLayout_);
    textureSet_ = VK_NULL_HANDLE;
    draws_.clear();
    vkCtx_ = nullptr;
}

bool BlobShadowRenderer::createPipeline() {
    VkDevice device = vkCtx_->getDevice();
    VkShaderModule vert;
    VkShaderModule frag;
    if (!vert.loadFromFile(device, "assets/shaders/blob_shadow.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/blob_shadow.frag.spv")) {
        vert.destroy();
        frag.destroy();
        return false;
    }
    VkVertexInputBindingDescription binding{.binding = 0, .stride = sizeof(glm::vec3),
                                            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription pos{.location = 0, .binding = 0,
                                          .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
    // Blend Mod (0x007e4480 sets state 6 to 4): the ground times the colour,
    // its alpha left alone. Depth tested, not written (state 15 = 0), drawn
    // a little nearer than the ground it repeats (0x00763c70).
    VkPipelineColorBlendAttachmentState mod = PipelineBuilder::blendAdditive();
    mod.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mod.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    mod.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mod.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    pipeline_ = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({binding}, {pos})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setDepthBias(-1.0f, -1.0f)
        .setColorBlendAttachment(mod)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vert.destroy();
    frag.destroy();
    return pipeline_ != VK_NULL_HANDLE;
}

void BlobShadowRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    if (!createPipeline()) LOG_WARNING("BlobShadowRenderer: pipeline recreation failed");
}

void BlobShadowRenderer::prepare(uint32_t frameIndex, const std::vector<blob_shadow::Caster>& casters,
                                 const glm::mat4& viewProj, const TerrainManager* terrain,
                                 const WMORenderer* wmo, const M2Renderer* m2) {
    draws_.clear();
    if (!vkCtx_ || !pipeline_) return;
    frame_ = frameIndex % MAX_FRAMES_IN_FLIGHT;
    auto* dst = static_cast<glm::vec3*>(vertexMapped_[frame_]);
    if (!dst) return;
    Frustum frustum;
    frustum.extractFromMatrix(viewProj);

    uint32_t written = 0;
    for (const blob_shadow::Caster& caster : casters) {
        if (blob_shadow::isEmpty(caster.box)) continue;
        const auto projection = blob_shadow::project(caster.box, caster.world);
        if (!projection || !frustum.intersectsAABB(projection->boxMin, projection->boxMax)) continue;

        // The ground under it: terrain, then buildings, then doodads
        // (0x007e35f0 through 0x0077f340, 0x007a6af0).
        scratch_.clear();
        if (terrain) terrain->gatherBlobShadowGround(projection->boxMin, projection->boxMax, scratch_);
        if (wmo) wmo->gatherBlobShadowGround(projection->boxMin, projection->boxMax, scratch_);
        if (m2) m2->gatherBlobShadowGround(projection->boxMin, projection->boxMax, scratch_);
        if (scratch_.empty()) continue;
        const uint32_t count = static_cast<uint32_t>(
            std::min<size_t>(scratch_.size() - scratch_.size() % 3, kMaxVertices - written));
        if (count == 0) break;
        std::memcpy(dst + written, scratch_.data(), count * sizeof(glm::vec3));

        Draw d;
        d.push.uRow = projection->uRow;
        d.push.vRow = projection->vRow;
        d.push.hRow = projection->hRow;
        d.push.params = glm::vec4(caster.alpha, 0.0f, 0.0f, 0.0f);
        d.firstVertex = written;
        d.vertexCount = count;
        draws_.push_back(d);
        written += count;
    }
}

void BlobShadowRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (draws_.empty() || !pipeline_ || !vertexBuffer_[frame_]) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDescriptorSet sets[2] = {perFrameSet, textureSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_[frame_], &offset);
    for (const Draw& d : draws_) {
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(Push), &d.push);
        vkCmdDraw(cmd, d.vertexCount, 1, d.firstVertex, 0);
    }
}

}  // namespace wowee::rendering
