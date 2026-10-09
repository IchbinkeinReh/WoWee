#include "rendering/blob_shadow_renderer.hpp"

#include "core/logger.hpp"
#include "game/ground_target.hpp"
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
    size.descriptorCount = 4;
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 4;
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
    auto bindTexture = [&](VkDescriptorSet set, const VkTexture& texture) {
        VkDescriptorImageInfo imageInfo = texture.descriptorInfo();
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    bindTexture(textureSet_, blobTexture_);

    // 0x007460c0 loads the circle's texture once, for every object; clamped
    // like the blob's, since the ground's triangles run past its box.
    pipeline::BLPImage circle = assets->loadTexture("Textures\\UnitSelectTexture.blp");
    if (circle.isValid() &&
        selectionTexture_.upload(*vkCtx_, circle.data.data(), circle.width, circle.height,
                                 VK_FORMAT_R8G8B8A8_UNORM, true)) {
        selectionTexture_.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (selectionTexture_.isValid() &&
            vkAllocateDescriptorSets(device, &alloc, &selectionSet_) == VK_SUCCESS) {
            bindTexture(selectionSet_, selectionTexture_);
        } else {
            selectionSet_ = VK_NULL_HANDLE;
        }
    }
    if (selectionSet_ == VK_NULL_HANDLE) {
        LOG_WARNING("BlobShadowRenderer: Textures\\UnitSelectTexture.blp unavailable; no selection circle");
    }

    // The spell's two circles (0x00ac799c), loaded once at the world frame's
    // creation, clamped like the others.
    const char* spellTargetPaths[2] = {game::ground_target::kAcceptableTexture,
                                       game::ground_target::kUnacceptableTexture};
    for (int i = 0; i < 2; ++i) {
        pipeline::BLPImage art = assets->loadTexture(spellTargetPaths[i]);
        if (!art.isValid() ||
            !spellTargetTextures_[i].upload(*vkCtx_, art.data.data(), art.width, art.height,
                                            VK_FORMAT_R8G8B8A8_UNORM, true)) {
            LOG_WARNING("BlobShadowRenderer: ", spellTargetPaths[i], " unavailable");
            continue;
        }
        spellTargetTextures_[i].createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (spellTargetTextures_[i].isValid() &&
            vkAllocateDescriptorSets(device, &alloc, &spellTargetSets_[i]) == VK_SUCCESS) {
            bindTexture(spellTargetSets_[i], spellTargetTextures_[i]);
        } else {
            spellTargetSets_[i] = VK_NULL_HANDLE;
        }
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(Push);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, textureLayout_}, {push});
    if (!pipelineLayout_ || !createPipeline()) {
        shutdown();
        return false;
    }
    if (selectionSet_ != VK_NULL_HANDLE && !createSelectionPipeline()) {
        LOG_WARNING("BlobShadowRenderer: selection circle pipeline failed");
    }
    if (!createSpellTargetPipeline()) {
        LOG_WARNING("BlobShadowRenderer: spell target circle pipeline failed");
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
    selectionTexture_.destroy(device, allocator);
    destroy(device, selectionPipeline_);
    selectionSet_ = VK_NULL_HANDLE;
    selectionDraw_.reset();
    for (int i = 0; i < 2; ++i) {
        spellTargetTextures_[i].destroy(device, allocator);
        spellTargetSets_[i] = VK_NULL_HANDLE;
    }
    destroy(device, spellTargetPipeline_);
    spellTargetDraw_.reset();
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

bool BlobShadowRenderer::createSelectionPipeline() {
    VkDevice device = vkCtx_->getDevice();
    VkShaderModule vert;
    VkShaderModule frag;
    if (!vert.loadFromFile(device, "assets/shaders/blob_shadow.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/selection_circle.frag.spv")) {
        vert.destroy();
        frag.destroy();
        return false;
    }
    VkVertexInputBindingDescription binding{.binding = 0, .stride = sizeof(glm::vec3),
                                            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription pos{.location = 0, .binding = 0,
                                          .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
    // Blend mode 3 (0x00744eb0 sets state 6 to 3): source alpha and one, the
    // alpha left alone. Depth tested and not written (states 0x11, 0xf = 0),
    // with the same nudge nearer as the blob (0.4 through 0x007e4370).
    VkPipelineColorBlendAttachmentState add = PipelineBuilder::blendAdditive();
    add.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    selectionPipeline_ = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({binding}, {pos})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setDepthBias(-1.0f, -1.0f)
        .setColorBlendAttachment(add)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vert.destroy();
    frag.destroy();
    return selectionPipeline_ != VK_NULL_HANDLE;
}

bool BlobShadowRenderer::createSpellTargetPipeline() {
    VkDevice device = vkCtx_->getDevice();
    VkShaderModule vert;
    VkShaderModule frag;
    if (!vert.loadFromFile(device, "assets/shaders/blob_shadow.vert.spv") ||
        !frag.loadFromFile(device, "assets/shaders/spell_target.frag.spv")) {
        vert.destroy();
        frag.destroy();
        return false;
    }
    VkVertexInputBindingDescription binding{.binding = 0, .stride = sizeof(glm::vec3),
                                            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription pos{.location = 0, .binding = 0,
                                          .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
    // Blend mode 2 (0x004f8a40 sets state 6 to 2): source alpha over one minus
    // it, the alpha left alone. Depth tested and not written (states 0xb, 0xf
    // = 0), with the projector's nudge nearer (0.4 through 0x007e4370).
    VkPipelineColorBlendAttachmentState alpha = PipelineBuilder::blendAdditive();
    alpha.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    alpha.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alpha.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    alpha.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    spellTargetPipeline_ = PipelineBuilder()
        .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({binding}, {pos})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setDepthBias(-1.0f, -1.0f)
        .setColorBlendAttachment(alpha)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    vert.destroy();
    frag.destroy();
    return spellTargetPipeline_ != VK_NULL_HANDLE;
}

void BlobShadowRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    destroy(vkCtx_->getDevice(), selectionPipeline_);
    destroy(vkCtx_->getDevice(), spellTargetPipeline_);
    if (!createSpellTargetPipeline()) LOG_WARNING("BlobShadowRenderer: spell target pipeline recreation failed");
    if (!createPipeline()) LOG_WARNING("BlobShadowRenderer: pipeline recreation failed");
    if (selectionSet_ != VK_NULL_HANDLE && !createSelectionPipeline()) {
        LOG_WARNING("BlobShadowRenderer: selection circle pipeline recreation failed");
    }
}

std::optional<BlobShadowRenderer::Draw> BlobShadowRenderer::gather(const blob_shadow::Projection& projection,
                                                                   const TerrainManager* terrain,
                                                                   const WMORenderer* wmo, const M2Renderer* m2,
                                                                   glm::vec3* dst) {
    // The ground in the box: terrain, then buildings, then doodads
    // (0x007e35f0 through 0x0077f340, 0x007a6af0).
    scratch_.clear();
    if (terrain) terrain->gatherBlobShadowGround(projection.boxMin, projection.boxMax, scratch_);
    if (wmo) wmo->gatherBlobShadowGround(projection.boxMin, projection.boxMax, scratch_);
    if (m2) m2->gatherBlobShadowGround(projection.boxMin, projection.boxMax, scratch_);
    if (scratch_.empty()) return std::nullopt;
    const uint32_t count = static_cast<uint32_t>(
        std::min<size_t>(scratch_.size() - scratch_.size() % 3, kMaxVertices - written_));
    if (count == 0) return std::nullopt;
    std::memcpy(dst + written_, scratch_.data(), count * sizeof(glm::vec3));
    Draw d;
    d.push.uRow = projection.uRow;
    d.push.vRow = projection.vRow;
    d.push.hRow = projection.hRow;
    d.firstVertex = written_;
    d.vertexCount = count;
    written_ += count;
    return d;
}

void BlobShadowRenderer::prepare(uint32_t frameIndex, const std::vector<blob_shadow::Caster>& casters,
                                 const glm::mat4& viewProj, const TerrainManager* terrain,
                                 const WMORenderer* wmo, const M2Renderer* m2) {
    draws_.clear();
    selectionDraw_.reset();
    spellTargetDraw_.reset();
    written_ = 0;
    frame_ = frameIndex % MAX_FRAMES_IN_FLIGHT;
    if (!vkCtx_ || !pipeline_) return;
    auto* dst = static_cast<glm::vec3*>(vertexMapped_[frame_]);
    if (!dst) return;
    Frustum frustum;
    frustum.extractFromMatrix(viewProj);

    for (const blob_shadow::Caster& caster : casters) {
        if (blob_shadow::isEmpty(caster.box)) continue;
        const auto projection = blob_shadow::project(caster.box, caster.world);
        if (!projection || !frustum.intersectsAABB(projection->boxMin, projection->boxMax)) continue;
        auto d = gather(*projection, terrain, wmo, m2, dst);
        if (!d) {
            if (written_ >= kMaxVertices) break;
            continue;
        }
        d->push.params = glm::vec4(caster.alpha, 0.0f, 0.0f, 0.0f);
        draws_.push_back(*d);
    }
}

void BlobShadowRenderer::prepareSelection(const std::optional<blob_shadow::Projection>& circle,
                                          const glm::vec4& color, const glm::mat4& viewProj,
                                          const TerrainManager* terrain, const WMORenderer* wmo,
                                          const M2Renderer* m2) {
    selectionDraw_.reset();
    if (!circle || !vkCtx_ || !selectionPipeline_ || selectionSet_ == VK_NULL_HANDLE) return;
    auto* dst = static_cast<glm::vec3*>(vertexMapped_[frame_]);
    if (!dst) return;
    Frustum frustum;
    frustum.extractFromMatrix(viewProj);
    if (!frustum.intersectsAABB(circle->boxMin, circle->boxMax)) return;
    selectionDraw_ = gather(*circle, terrain, wmo, m2, dst);
    if (selectionDraw_) selectionDraw_->push.params = color;
}

void BlobShadowRenderer::prepareSpellTarget(const std::optional<blob_shadow::Projection>& circle,
                                            bool unacceptable, const glm::mat4& viewProj,
                                            const TerrainManager* terrain, const WMORenderer* wmo,
                                            const M2Renderer* m2) {
    spellTargetDraw_.reset();
    spellTargetTexture_ = unacceptable ? 1 : 0;
    if (!circle || !vkCtx_ || !spellTargetPipeline_ || spellTargetSets_[spellTargetTexture_] == VK_NULL_HANDLE)
        return;
    auto* dst = static_cast<glm::vec3*>(vertexMapped_[frame_]);
    if (!dst) return;
    Frustum frustum;
    frustum.extractFromMatrix(viewProj);
    if (!frustum.intersectsAABB(circle->boxMin, circle->boxMax)) return;
    spellTargetDraw_ = gather(*circle, terrain, wmo, m2, dst);
    if (spellTargetDraw_) spellTargetDraw_->push.params = glm::vec4(1.0f);
}

void BlobShadowRenderer::renderSpellTarget(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!spellTargetDraw_ || !spellTargetPipeline_ || !vertexBuffer_[frame_]) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, spellTargetPipeline_);
    const VkDescriptorSet sets[2] = {perFrameSet, spellTargetSets_[spellTargetTexture_]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_[frame_], &offset);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(Push), &spellTargetDraw_->push);
    vkCmdDraw(cmd, spellTargetDraw_->vertexCount, 1, spellTargetDraw_->firstVertex, 0);
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

void BlobShadowRenderer::renderSelection(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!selectionDraw_ || !selectionPipeline_ || !vertexBuffer_[frame_]) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selectionPipeline_);
    const VkDescriptorSet sets[2] = {perFrameSet, selectionSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_[frame_], &offset);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(Push), &selectionDraw_->push);
    vkCmdDraw(cmd, selectionDraw_->vertexCount, 1, selectionDraw_->firstVertex, 0);
}

}  // namespace wowee::rendering
