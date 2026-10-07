#include "rendering/screen_effects.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <algorithm>

namespace wowee {
namespace rendering {

namespace {

constexpr VkFormat kCopyFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kGlowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBlurGroup = 8;  // 8x8, screen_glow.comp.glsl

/// Must match Push in screen_effects_composite.frag.glsl.
struct CompositePush {
    glm::vec4 params;    // x = glow, y = death
    glm::vec4 keepRect;  // the minimap, uv x, y, w, h
};

}  // namespace

ScreenEffects::~ScreenEffects() {
    shutdown();
}

bool ScreenEffects::initialize(VkContext* ctx) {
    if (!ctx) return false;
    ctx_ = ctx;
    VkDevice device = ctx_->getDevice();

    const auto has = [&](VkFormat format, VkFormatFeatureFlags needed) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), format, &props);
        return (props.optimalTilingFeatures & needed) == needed;
    };
    if (!has(ctx_->getSwapchainFormat(), VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !has(kCopyFormat, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
        !has(kGlowFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_WARNING("ScreenEffects: the swapchain cannot be copied or the glow stored here"
                    " - glow and the death effect unavailable");
        return false;
    }

    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx_->getOrCreateSampler(sampCI);
    if (sampler_ == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayoutBinding blur[2]{};
    blur[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    blur[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    blurSetLayout_ = createDescriptorSetLayout(device, {blur[0], blur[1]});
    VkDescriptorSetLayoutBinding comp[2]{};
    comp[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    comp[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    compositeSetLayout_ = createDescriptorSetLayout(device, {comp[0], comp[1]});
    if (blurSetLayout_ == VK_NULL_HANDLE || compositeSetLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("ScreenEffects: failed to create descriptor set layouts");
        return false;
    }

    blurLayout_ = createPipelineLayout(device, {blurSetLayout_});
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0,
                                   .size = sizeof(CompositePush)};
    compositeLayout_ = createPipelineLayout(device, {compositeSetLayout_}, {push});
    if (blurLayout_ == VK_NULL_HANDLE || compositeLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("ScreenEffects: failed to create pipeline layouts");
        return false;
    }

    {
        VkShaderModule module;
        if (!module.loadFromFile(device, "assets/shaders/screen_glow.comp.spv")) {
            LOG_ERROR("ScreenEffects: failed to load screen_glow.comp.spv");
            return false;
        }
        VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = blurLayout_;
        const bool ok = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI,
                                                 nullptr, &blurPipeline_) == VK_SUCCESS;
        module.destroy();
        if (!ok) {
            LOG_ERROR("ScreenEffects: failed to create the blur pipeline");
            return false;
        }
    }

    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES * 3},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = MAX_FRAMES},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES * 2;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("ScreenEffects: failed to create descriptor pool");
        return false;
    }
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descPool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &blurSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &blurSets_[i]) != VK_SUCCESS) return false;
        alloc.pSetLayouts = &compositeSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &compositeSets_[i]) != VK_SUCCESS) return false;
    }

    if (!ensureCompositePipeline()) return false;

    usable_ = true;
    return true;
}

bool ScreenEffects::ensureCompositePipeline() {
    const VkFormat format = ctx_->getSwapchainFormat();
    if (compositePipeline_ != VK_NULL_HANDLE && format == compositeFormat_) return true;
    VkDevice device = ctx_->getDevice();
    if (compositePipeline_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
    destroy(device, compositePipeline_);

    ShaderPair shaders = loadShaderPair(device, "assets/shaders/postprocess.vert.spv",
                                        "assets/shaders/screen_effects_composite.frag.spv",
                                        "screen effects");
    if (!shaders) return false;

    // The frame is drawn again whole, from its own copy: no blending.
    VkPipelineColorBlendAttachmentState opaque{};
    opaque.blendEnable = VK_FALSE;
    opaque.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    compositePipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(opaque)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(compositeLayout_)
        .setRenderPass(ctx_->getOverlayRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, ctx_->getPipelineCache());
    if (compositePipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("ScreenEffects: failed to create the composite pipeline");
        return false;
    }
    compositeFormat_ = format;
    return true;
}

void ScreenEffects::destroyTargets() {
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroyScreenTarget(*ctx_, frameCopy_[i]);
        destroyScreenTarget(*ctx_, smallCopy_[i]);
        destroyScreenTarget(*ctx_, glow_[i]);
        drawThisFrame_[i] = false;
    }
    smallExtent_ = {.width = 0, .height = 0};
    sourceExtent_ = {.width = 0, .height = 0};
}

bool ScreenEffects::ensureTargets(VkExtent2D extent) {
    if (extent.width == sourceExtent_.width && extent.height == sourceExtent_.height &&
        glow_[0].image != VK_NULL_HANDLE) {
        return true;
    }
    vkDeviceWaitIdle(ctx_->getDevice());
    destroyTargets();

    smallExtent_ = {.width = std::max(1u, extent.width / 4), .height = std::max(1u, extent.height / 4)};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (!createScreenTarget(*ctx_, frameCopy_[i], extent, kCopyFormat,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !createScreenTarget(*ctx_, smallCopy_[i], smallExtent_, kCopyFormat,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !createScreenTarget(*ctx_, glow_[i], smallExtent_, kGlowFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            LOG_WARNING("ScreenEffects: could not allocate targets for ", extent.width, "x",
                        extent.height, " - glow and the death effect off");
            destroyTargets();
            usable_ = false;
            return false;
        }
    }

    // The glow lives in GENERAL, written by the blur and sampled by the
    // composite from the same layout.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b[MAX_FRAMES]{};
        for (uint32_t i = 0; i < MAX_FRAMES; i++) {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = glow_[i].image;
            b[i].subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = MAX_FRAMES;
        dep.pImageMemoryBarriers = b;
        cmdPipelineBarrier2(cmd, dep);
    });

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorImageInfo smallIn{.sampler = sampler_, .imageView = smallCopy_[i].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo glowOut{.sampler = VK_NULL_HANDLE, .imageView = glow_[i].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo frameIn{.sampler = sampler_, .imageView = frameCopy_[i].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo glowIn{.sampler = sampler_, .imageView = glow_[i].view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[4]{};
        for (auto& write : w) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.descriptorCount = 1;
        }
        w[0].dstSet = blurSets_[i];
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &smallIn;
        w[1].dstSet = blurSets_[i];
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &glowOut;
        w[2].dstSet = compositeSets_[i];
        w[2].dstBinding = 0;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &frameIn;
        w[3].dstSet = compositeSets_[i];
        w[3].dstBinding = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = &glowIn;
        vkUpdateDescriptorSets(ctx_->getDevice(), 4, w, 0, nullptr);
    }
    sourceExtent_ = extent;
    return true;
}

bool ScreenEffects::record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                           VkExtent2D extent, const FrameInputs& in) {
    ZoneScopedN("ScreenEffects::record");
    if (frame >= MAX_FRAMES) return false;
    drawThisFrame_[frame] = false;
    if (!usable_ || (in.glow <= 0.0f && in.death <= 0.0f) || cmd == VK_NULL_HANDLE ||
        swapchainImage == VK_NULL_HANDLE || extent.width < 4 || extent.height < 4) {
        return false;
    }
    if (!ensureTargets(extent) || !ensureCompositePipeline()) return false;

    const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };

    barrier(swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    // The copies' last readers were this slot's composite and blur, two
    // frames ago.
    barrier(frameCopy_[frame].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier(smallCopy_[frame].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   frameCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    blit.dstOffsets[1] = {.x = static_cast<int32_t>(smallExtent_.width),
                          .y = static_cast<int32_t>(smallExtent_.height), .z = 1};
    vkCmdBlitImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   smallCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    barrier(swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(frameCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier(smallCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier(glow_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurLayout_, 0, 1, &blurSets_[frame], 0, nullptr);
    vkCmdDispatch(cmd, (smallExtent_.width + kBlurGroup - 1) / kBlurGroup,
                  (smallExtent_.height + kBlurGroup - 1) / kBlurGroup, 1);

    barrier(glow_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    inputs_[frame] = in;
    drawThisFrame_[frame] = true;
    return true;
}

void ScreenEffects::composite(VkCommandBuffer cmd, uint32_t frame) {
    if (frame >= MAX_FRAMES || !drawThisFrame_[frame] || compositePipeline_ == VK_NULL_HANDLE) return;
    CompositePush push{};
    push.params = glm::vec4(inputs_[frame].glow, inputs_[frame].death, 0.0f, 0.0f);
    push.keepRect = inputs_[frame].keepRect;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositeLayout_, 0, 1,
                            &compositeSets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, compositeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void ScreenEffects::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    vkDeviceWaitIdle(device);
    destroyTargets();
    destroy(device, descPool_);
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        blurSets_[i] = VK_NULL_HANDLE;
        compositeSets_[i] = VK_NULL_HANDLE;
    }
    destroy(device, blurPipeline_);
    destroy(device, compositePipeline_);
    destroy(device, blurLayout_);
    destroy(device, compositeLayout_);
    destroy(device, blurSetLayout_);
    destroy(device, compositeSetLayout_);
    sampler_ = VK_NULL_HANDLE;  // the context's cache owns it
    compositeFormat_ = VK_FORMAT_UNDEFINED;
    usable_ = false;
    ctx_ = nullptr;
}

} // namespace rendering
} // namespace wowee
