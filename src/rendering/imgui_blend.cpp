#include "rendering/imgui_blend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <cstddef>
#include <vector>

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"

namespace wowee {
namespace rendering {

namespace {

struct BlendState {
    VkContext* ctx = nullptr;
    VkPipeline additive = VK_NULL_HANDLE;
    VkRenderPass builtFor = VK_NULL_HANDLE;
    VkPipelineLayout builtWith = VK_NULL_HANDLE;
    // Pipelines built for a render pass since replaced. A frame in flight may
    // still use one, so they go when the interface does.
    std::vector<VkPipeline> retired;
    bool failed = false;
};
BlendState g_blend;

VkPipeline buildAdditive(VkDevice device, VkRenderPass pass, VkPipelineLayout layout) {
    ShaderPair shaders = loadShaderPair(device, "assets/shaders/imgui_add.vert.spv",
                                        "assets/shaders/imgui_add.frag.spv", "additive interface");
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
        .setColorBlendAttachment(PipelineBuilder::blendAdditive())
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(layout)
        .setRenderPass(pass)
        .build(device);
}

void bindAdditive(const ImDrawList*, const ImDrawCmd*) {
    auto* rs = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (!rs || !g_blend.ctx || g_blend.failed) return;
    const VkRenderPass pass = g_blend.ctx->getOverlayRenderPass();
    if (g_blend.additive == VK_NULL_HANDLE || g_blend.builtFor != pass || g_blend.builtWith != rs->PipelineLayout) {
        if (g_blend.additive != VK_NULL_HANDLE) g_blend.retired.push_back(g_blend.additive);
        g_blend.additive = buildAdditive(g_blend.ctx->getDevice(), pass, rs->PipelineLayout);
        g_blend.builtFor = pass;
        g_blend.builtWith = rs->PipelineLayout;
        if (g_blend.additive == VK_NULL_HANDLE) {
            LOG_ERROR("Additive interface pipeline could not be built; glows are blended instead");
            g_blend.failed = true;
            return;
        }
    }
    // Same layout as ImGui's, so its push constants and the descriptor set
    // the next command binds carry over.
    vkCmdBindPipeline(rs->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_blend.additive);
}

}  // namespace

void initImGuiBlend(VkContext* ctx) { g_blend.ctx = ctx; }

void shutdownImGuiBlend() {
    if (!g_blend.ctx) return;
    VkDevice device = g_blend.ctx->getDevice();
    if (g_blend.additive != VK_NULL_HANDLE) vkDestroyPipeline(device, g_blend.additive, nullptr);
    for (VkPipeline p : g_blend.retired) vkDestroyPipeline(device, p, nullptr);
    g_blend = BlendState{};
}

void beginAdditive(ImDrawList* list) { list->AddCallback(bindAdditive, nullptr); }

void endAdditive(ImDrawList* list) { list->AddCallback(ImDrawCallback_ResetRenderState, nullptr); }

}  // namespace rendering
}  // namespace wowee
