#include "rendering/world_text_renderer.hpp"

#include "core/logger.hpp"
#include "rendering/depth_convention.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace wowee::rendering {

namespace {
constexpr const char* kVert = "assets/shaders/world_text.vert.spv";
constexpr const char* kFrag = "assets/shaders/world_text.frag.spv";
}  // namespace

WorldTextRenderer::~WorldTextRenderer() { shutdown(); }

bool WorldTextRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    if (!ctx) return false;
    if (vkCtx_) return true;
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    VkDevice device = vkCtx_->getDevice();

    // The interface's texture sets are made against ImGui's layout - one
    // combined image sampler at binding 0, for the fragment stage - and a
    // layout defined the same way is compatible with them, so the font's own
    // set binds here as it is.
    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureLayout_ = createDescriptorSetLayout(device, {sampler});
    if (textureLayout_ == VK_NULL_HANDLE) {
        shutdown();
        return false;
    }
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, textureLayout_});
    if (pipelineLayout_ == VK_NULL_HANDLE) {
        shutdown();
        return false;
    }
    buildPipeline();
    if (pipeline_ == VK_NULL_HANDLE) {
        shutdown();
        return false;
    }
    for (uint32_t f = 0; f < kFrames; ++f) {
        AllocatedBuffer buf = createBuffer(vkCtx_->getAllocator(), kMaxVertices * sizeof(Vertex),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        vertexBuffer_[f] = buf.buffer;
        vertexAlloc_[f] = buf.allocation;
        vertexMapped_[f] = buf.info.pMappedData;
        if (!vertexBuffer_[f] || !vertexMapped_[f]) {
            shutdown();
            return false;
        }
    }
    return true;
}

/// Alpha blended, depth tested and written, both sides: a world string batch
/// (0x006c53a0 - blend 2, states 0xd and 0xf on, culling off). The alpha
/// reference of 1 is the fragment shader's discard.
void WorldTextRenderer::buildPipeline() {
    VkDevice device = vkCtx_->getDevice();
    auto shaders = loadShaderPair(device, kVert, kFrag, "world text");
    if (!shaders) return;
    const std::vector<VkVertexInputAttributeDescription> attributes = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, anchor))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, offset))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(Vertex, uv))},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .offset = static_cast<uint32_t>(offsetof(Vertex, colour))},
    };
    pipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({tightVertexBinding(sizeof(Vertex))}, attributes)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, depth::kNearerOrEqual)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
}

void WorldTextRenderer::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    for (uint32_t f = 0; f < kFrames; ++f) {
        destroy(vkCtx_->getAllocator(), vertexBuffer_[f], vertexAlloc_[f]);
        vertexMapped_[f] = nullptr;
    }
    destroyPipeline(device, pipeline_, pipelineLayout_);
    destroy(device, textureLayout_);
    vkCtx_ = nullptr;
    triangles_.clear();
    atlas_ = nullptr;
}

void WorldTextRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    buildPipeline();
}

void WorldTextRenderer::setText(std::vector<Vertex> triangles, const ImFontAtlas* atlas) {
    triangles_ = std::move(triangles);
    atlas_ = atlas;
}

void WorldTextRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (triangles_.empty() || !atlas_ || pipeline_ == VK_NULL_HANDLE) return;
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ImTextureID carries the set.
    const auto fontTexture = reinterpret_cast<VkDescriptorSet>(atlas_->TexRef.GetTexID());
    if (fontTexture == VK_NULL_HANDLE) return;
    const uint32_t frame = vkCtx_->getCurrentFrame() % kFrames;
    if (!vertexMapped_[frame]) return;
    // Whole triangles only.
    const size_t count = std::min(triangles_.size(), kMaxVertices) / 3 * 3;
    if (count == 0) return;
    std::memcpy(vertexMapped_[frame], triangles_.data(), count * sizeof(Vertex));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDescriptorSet sets[2] = {perFrameSet, fontTexture};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_[frame], &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);
    // Drawn once: the interface lays the text out again every frame, and a
    // frame it does not - out of the world - leaves none standing.
    triangles_.clear();
}

}  // namespace wowee::rendering
