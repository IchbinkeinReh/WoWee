#include "rendering/fishing_line.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"

#include <cstring>
#include <algorithm>

namespace wowee::rendering {

namespace {
constexpr const char* kVert = "assets/shaders/fishing_line.vert.spv";
constexpr const char* kFrag = "assets/shaders/fishing_line.frag.spv";

struct LinePush {
    glm::vec4 colour;
};
}  // namespace

FishingLineRenderer::~FishingLineRenderer() { shutdown(); }

/// A line strip of positions, depth tested and written, opaque: the
/// client's untextured strip of 64 lines (0x006f8f50, primitive 2).
void FishingLineRenderer::buildPipeline(VkDevice device, const VkPipelineShaderStageCreateInfo& vertStage,
                                        const VkPipelineShaderStageCreateInfo& fragStage) {
    const VkVertexInputAttributeDescription pos{
        .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
    pipeline_ = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({tightVertexBinding(sizeof(glm::vec3))}, {pos})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
}

bool FishingLineRenderer::initialize(Renderer* owner, VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    owner_ = owner;
    vkCtx_ = ctx;
    VkDevice device = vkCtx_->getDevice();
    auto shaders = loadShaderPair(device, kVert, kFrag, "fishing_line");
    if (!shaders) return false;
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(LinePush);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout}, {range});
    if (pipelineLayout_ == VK_NULL_HANDLE) return false;
    buildPipeline(device, shaders.vertStage, shaders.fragStage);
    if (pipeline_ == VK_NULL_HANDLE) return false;

    const VkDeviceSize size = kMaxLines * fishing_line::kPoints * sizeof(glm::vec3);
    AllocatedBuffer buf = createBuffer(vkCtx_->getAllocator(), size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       VMA_MEMORY_USAGE_CPU_TO_GPU);
    vertexBuffer_ = buf.buffer;
    vertexAlloc_ = buf.allocation;
    vertexAllocInfo_ = buf.info;
    return vertexBuffer_ != VK_NULL_HANDLE;
}

void FishingLineRenderer::shutdown() {
    if (!vkCtx_) return;
    destroyParticleResources(vkCtx_->getDevice(), vkCtx_->getAllocator(), pipeline_, pipelineLayout_,
                             vertexBuffer_, vertexAlloc_);
    vkCtx_ = nullptr;
    lines_.clear();
}

void FishingLineRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, pipeline_);
    auto shaders = loadShaderPair(device, kVert, kFrag, "fishing_line");
    if (!shaders) return;
    buildPipeline(device, shaders.vertStage, shaders.fragStage);
}

void FishingLineRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (lines_.empty() || pipeline_ == VK_NULL_HANDLE || !vertexAllocInfo_.pMappedData || !owner_) return;
    auto* characters = owner_->getCharacterRenderer();
    auto* m2 = owner_->getM2Renderer();
    if (!characters || !m2) return;
    auto* out = static_cast<glm::vec3*>(vertexAllocInfo_.pMappedData);
    std::vector<glm::vec3> colours;
    colours.reserve(lines_.size());
    for (const Line& line : lines_) {
        if (colours.size() >= kMaxLines) break;
        // The pole's tip, its "$CCH" (0x006f8f50).
        const auto tip = characters->heldModelEvent(line.unitInstance, 1, fishing_line::kTipEvent);
        if (!tip) continue;
        glm::vec3 position;
        float scale = 1.0f;
        float height = 0.0f;
        if (!m2->getInstanceHeight(line.bobberInstance, position, scale, height)) continue;
        const auto pts = fishing_line::points(tip->position, fishing_line::bobberEnd(position, scale, height));
        std::memcpy(out + colours.size() * fishing_line::kPoints, pts.data(), sizeof(pts));
        colours.push_back(fishing_line::colour(tip->ambient));
    }
    if (colours.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &perFrameSet, 0,
                            nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
    for (size_t i = 0; i < colours.size(); ++i) {
        const LinePush push{glm::vec4(colours[i], 1.0f)};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, fishing_line::kPoints, 1, static_cast<uint32_t>(i * fishing_line::kPoints), 0);
    }
}

}  // namespace wowee::rendering
