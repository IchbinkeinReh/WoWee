#include "rendering/overlay_system.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

OverlaySystem::OverlaySystem(VkContext* ctx)
    : vkCtx_(ctx) {}

OverlaySystem::~OverlaySystem() {
    cleanup();
}

void OverlaySystem::cleanup() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, overlayPipeline_);
    destroy(device, brightnessPipeline_);
    destroy(device, overlayPipelineLayout_);
}

void OverlaySystem::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, overlayPipeline_);
    destroy(device, brightnessPipeline_);
}

void OverlaySystem::initOverlayPipeline() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 112;  // mat4 invViewProj + vec4 colour + vec4 plane + vec4 params

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(device, &plCI, nullptr, &overlayPipelineLayout_);

    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/overlay.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load overlay shaders");
        vertMod.destroy(); fragMod.destroy();
        return;
    }

    overlayPipeline_ = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(overlayPipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertMod.destroy(); fragMod.destroy();

    if (overlayPipeline_) LOG_INFO("OverlaySystem: overlay pipeline initialized");
}

namespace {

/// overlay.frag's push block. One definition, because there are two entry
/// points into that shader and only one of them was keeping up with it.
struct OverlayPush {
    glm::mat4 invViewProj;
    glm::vec4 color;
    glm::vec4 plane;
    glm::vec4 params;
};
static_assert(sizeof(OverlayPush) == 112, "must match overlay.frag's push block");

OverlayPush makeOverlayPush(const glm::vec4& color, const glm::mat4& invViewProj,
                            float waterZ, float softness, float rippleAmp,
                            float time, bool hasSeam) {
    OverlayPush p{};
    p.invViewProj = invViewProj;
    p.color = color;
    p.plane = glm::vec4(0.0f, 0.0f, 0.0f, waterZ);
    p.params = glm::vec4(softness, time, rippleAmp, hasSeam ? 1.0f : 0.0f);
    return p;
}

}  // namespace

void OverlaySystem::renderOverlay(const glm::vec4& color, VkCommandBuffer cmd) {
    // Whole screen: no seam, so the shader skips the surface test entirely and
    // the matrix and height below are never read.
    renderWaterline(color, glm::mat4(1.0f), 0.0f, 0.0f, 0.0f, 0.0f, false, cmd);
}

void OverlaySystem::renderWaterline(const glm::vec4& color, const glm::mat4& invViewProj,
                                    float waterZ, float softness, float rippleAmp,
                                    float time, bool hasSeam, VkCommandBuffer cmd) {
    if (!overlayPipeline_) initOverlayPipeline();
    if (!overlayPipeline_ || cmd == VK_NULL_HANDLE) return;
    OverlayPush push = makeOverlayPush(color, invViewProj, waterZ, softness, rippleAmp,
                                       time, hasSeam);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
    vkCmdPushConstants(cmd, overlayPipelineLayout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void OverlaySystem::initBrightnessPipeline() {
    if (brightnessPipeline_ != VK_NULL_HANDLE) return;
    if (!vkCtx_) return;
    // Reuse the overlay pipeline's layout (same fragment push constant); only
    // the blend state differs.
    if (!overlayPipeline_) initOverlayPipeline();
    if (!overlayPipelineLayout_) return;
    VkDevice device = vkCtx_->getDevice();

    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/overlay.frag.spv")) {
        LOG_ERROR("OverlaySystem: failed to load brightness shaders");
        vertMod.destroy(); fragMod.destroy();
        return;
    }

    // result.rgb = src.rgb * dst.rgb + dst.rgb * 1 = dst.rgb * (1 + src.rgb).
    // Pushing src.rgb = (scale - 1) yields dst.rgb * scale - a true luminance
    // multiply. Alpha is left untouched (dst passes through).
    VkPipelineColorBlendAttachmentState mul{};
    mul.blendEnable = VK_TRUE;
    mul.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mul.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    mul.colorBlendOp = VK_BLEND_OP_ADD;
    mul.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mul.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    mul.alphaBlendOp = VK_BLEND_OP_ADD;
    mul.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    brightnessPipeline_ = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(mul)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(overlayPipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    vertMod.destroy(); fragMod.destroy();
    if (brightnessPipeline_) LOG_INFO("OverlaySystem: brightness pipeline initialized");
}

void OverlaySystem::renderBrightnessScale(float scale, VkCommandBuffer cmd) {
    if (scale <= 1.0f) return; // darkening handled by the black overlay path
    if (!brightnessPipeline_) initBrightnessPipeline();
    if (!brightnessPipeline_ || cmd == VK_NULL_HANDLE) return;
    // overlay.frag outputs the pushed color; the blend multiplies it by dst.
    //
    // The whole block, not just the colour. This pushed sixteen bytes back when
    // the block started with one, and kept doing it after a mat4 was put in
    // front: the colour landed in the matrix's first row and everything the
    // shader actually reads was whatever the last draw had left there. It runs
    // whenever brightness is above neutral, so the screen flashed green or red
    // by the frame.
    const glm::vec4 tint(scale - 1.0f, scale - 1.0f, scale - 1.0f, 1.0f);
    OverlayPush push = makeOverlayPush(tint, glm::mat4(1.0f), 0.0f, 0.0f, 0.0f, 0.0f, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, brightnessPipeline_);
    vkCmdPushConstants(cmd, overlayPipelineLayout_,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
