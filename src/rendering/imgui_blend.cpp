#include "rendering/imgui_blend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

namespace wowee {
namespace rendering {

namespace {

/// One more pipeline beside ImGui's own.
struct Extra {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkRenderPass builtFor = VK_NULL_HANDLE;
    VkPipelineLayout builtWith = VK_NULL_HANDLE;
    bool failed = false;
};
struct BlendState {
    VkContext* ctx = nullptr;
    std::array<Extra, 6> extra;   // by ImGuiBlend
    // Pipelines built for a render pass since replaced. A frame in flight may
    // still use one, so they go when the interface does.
    std::vector<VkPipeline> retired;
};
BlendState g_blend;

VkPipeline build(VkDevice device, VkRenderPass pass, VkPipelineLayout layout,
                 const VkPipelineColorBlendAttachmentState& blend, const char* fragment) {
    ShaderPair shaders = loadShaderPair(device, "assets/shaders/imgui_add.vert.spv", fragment, "interface");
    if (!shaders) return VK_NULL_HANDLE;
    // ImDrawVert, as imgui_impl_vulkan.cpp lays it out.
    const std::vector<VkVertexInputBindingDescription> bindings = {
        {.binding = 0, .stride = sizeof(ImDrawVert), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
    const std::vector<VkVertexInputAttributeDescription> attributes = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(ImDrawVert, pos))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = static_cast<uint32_t>(offsetof(ImDrawVert, uv))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .offset = static_cast<uint32_t>(offsetof(ImDrawVert, col))},
    };
    return PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput(bindings, attributes)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(blend)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(layout)
        .setRenderPass(pass)
        .build(device);
}

VkPipelineColorBlendAttachmentState stateOf(ImGuiBlend blend) {
    switch (blend) {
        case ImGuiBlend::Add: return PipelineBuilder::blendAdditive();
        case ImGuiBlend::Premultiplied: return PipelineBuilder::blendPremultiplied();
        case ImGuiBlend::Mod: {
            // GxBlend_Mod: what is there, times it; its alpha says nothing.
            VkPipelineColorBlendAttachmentState state = PipelineBuilder::blendAdditive();
            state.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            return state;
        }
        default: return PipelineBuilder::blendDisabled();
    }
}

void bindBlend(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto blend = static_cast<ImGuiBlend>(reinterpret_cast<uintptr_t>(cmd->UserCallbackData));
    Extra& extra = g_blend.extra[static_cast<size_t>(blend)];
    auto* rs = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (!rs || !g_blend.ctx || extra.failed) return;
    const VkRenderPass pass = g_blend.ctx->getOverlayRenderPass();
    if (extra.pipeline == VK_NULL_HANDLE || extra.builtFor != pass || extra.builtWith != rs->PipelineLayout) {
        if (extra.pipeline != VK_NULL_HANDLE) g_blend.retired.push_back(extra.pipeline);
        extra.pipeline = build(g_blend.ctx->getDevice(), pass, rs->PipelineLayout, stateOf(blend),
                               blend == ImGuiBlend::AlphaKey ? "assets/shaders/imgui_key.frag.spv"
                                                             : "assets/shaders/imgui_add.frag.spv");
        extra.builtFor = pass;
        extra.builtWith = rs->PipelineLayout;
        if (extra.pipeline == VK_NULL_HANDLE) {
            LOG_ERROR("An interface pipeline could not be built; what it is for is blended instead");
            extra.failed = true;
            return;
        }
    }
    // Same layout as ImGui's, so its push constants and the descriptor set
    // the next command binds carry over.
    vkCmdBindPipeline(rs->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, extra.pipeline);
}

}  // namespace

void initImGuiBlend(VkContext* ctx) { g_blend.ctx = ctx; }

void shutdownImGuiBlend() {
    if (!g_blend.ctx) return;
    VkDevice device = g_blend.ctx->getDevice();
    for (const Extra& e : g_blend.extra)
        if (e.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, e.pipeline, nullptr);
    for (VkPipeline p : g_blend.retired) vkDestroyPipeline(device, p, nullptr);
    g_blend = BlendState{};
}

void setImGuiBlend(ImDrawList* list, ImGuiBlend blend) {
    if (blend == ImGuiBlend::Blend) list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    else list->AddCallback(bindBlend, reinterpret_cast<void*>(static_cast<uintptr_t>(blend)));
}

}  // namespace rendering
}  // namespace wowee
