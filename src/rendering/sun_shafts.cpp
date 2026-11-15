#include "rendering/sun_shafts.hpp"
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
constexpr VkFormat kRaysFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kMarchGroup = 8;  // 8x8, sun_shafts.comp.glsl

/// Must match Push in sun_shafts.comp.glsl.
struct MarchPush {
    glm::vec4 sun;   // xy = the sun's place on screen, z = width / height, w = strength
    glm::vec4 tint;  // rgb = the sun's colour
};

}  // namespace

SunShafts::~SunShafts() {
    shutdown();
}

bool SunShafts::initialize(VkContext* ctx) {
    if (!ctx) return false;
    ctx_ = ctx;
    VkDevice device = ctx_->getDevice();

    // Every one of these is required of a Vulkan device, but a device that
    // breaks the rule should cost the rays and not the session.
    const auto has = [&](VkFormat format, VkFormatFeatureFlags needed) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), format, &props);
        return (props.optimalTilingFeatures & needed) == needed;
    };
    if (!has(ctx_->getSwapchainFormat(), VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !has(kCopyFormat, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
        !has(kRaysFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
        !has(ctx_->getDepthFormat(), VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        LOG_WARNING("SunShafts: the swapchain cannot be copied down or the ray image stored to here"
                    " - sun shafts unavailable");
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
    // The depth is fetched texel by texel, never filtered.
    sampCI.magFilter = VK_FILTER_NEAREST;
    sampCI.minFilter = VK_FILTER_NEAREST;
    pointSampler_ = ctx_->getOrCreateSampler(sampCI);
    if (sampler_ == VK_NULL_HANDLE || pointSampler_ == VK_NULL_HANDLE) return false;

    VkDescriptorSetLayoutBinding mask[3]{};
    mask[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    mask[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    mask[2] = {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
               .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    maskSetLayout_ = createDescriptorSetLayout(device, {mask[0], mask[1], mask[2]});

    VkDescriptorSetLayoutBinding march[2]{};
    march[0] = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    march[1] = {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    marchSetLayout_ = createDescriptorSetLayout(device, {march[0], march[1]});
    VkDescriptorSetLayoutBinding rays{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    compositeSetLayout_ = createDescriptorSetLayout(device, {rays});
    if (maskSetLayout_ == VK_NULL_HANDLE || marchSetLayout_ == VK_NULL_HANDLE ||
        compositeSetLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("SunShafts: failed to create descriptor set layouts");
        return false;
    }

    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0,
                                   .size = sizeof(MarchPush)};
    maskLayout_ = createPipelineLayout(device, {maskSetLayout_});
    marchLayout_ = createPipelineLayout(device, {marchSetLayout_}, {push});
    compositeLayout_ = createPipelineLayout(device, {compositeSetLayout_});
    if (maskLayout_ == VK_NULL_HANDLE || marchLayout_ == VK_NULL_HANDLE ||
        compositeLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("SunShafts: failed to create pipeline layouts");
        return false;
    }

    const auto computePipeline = [&](const char* path, VkPipelineLayout layout, VkPipeline& out) {
        VkShaderModule module;
        if (!module.loadFromFile(device, path)) {
            LOG_ERROR("SunShafts: failed to load ", path);
            return false;
        }
        VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = layout;
        const bool ok = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI,
                                                 nullptr, &out) == VK_SUCCESS;
        module.destroy();
        if (!ok) LOG_ERROR("SunShafts: failed to create the pipeline of ", path);
        return ok;
    };
    if (!computePipeline("assets/shaders/sun_shafts_mask.comp.spv", maskLayout_, maskPipeline_) ||
        !computePipeline("assets/shaders/sun_shafts_mask_ms.comp.spv", maskLayout_, maskPipelineMs_) ||
        !computePipeline("assets/shaders/sun_shafts.comp.spv", marchLayout_, marchPipeline_)) {
        return false;
    }

    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES * 4},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = MAX_FRAMES * 2},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES * 3;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("SunShafts: failed to create descriptor pool");
        return false;
    }
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descPool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &maskSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &maskSets_[i]) != VK_SUCCESS) return false;
        alloc.pSetLayouts = &marchSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &marchSets_[i]) != VK_SUCCESS) return false;
        alloc.pSetLayouts = &compositeSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &compositeSets_[i]) != VK_SUCCESS) return false;
    }

    // Now rather than at the first sunny frame, so a pipeline that will not
    // build says so at start-up and not partway into the world.
    if (!ensureCompositePipeline()) return false;

    usable_ = true;
    return true;
}

bool SunShafts::ensureCompositePipeline() {
    const VkFormat format = ctx_->getSwapchainFormat();
    if (compositePipeline_ != VK_NULL_HANDLE && format == compositeFormat_) return true;
    VkDevice device = ctx_->getDevice();
    // A rebuild replaces a pipeline an in-flight frame may have bound.
    if (compositePipeline_ != VK_NULL_HANDLE) ::wowee::rendering::deviceWaitIdle(device);
    destroy(device, compositePipeline_);

    ShaderPair shaders = loadShaderPair(device, "assets/shaders/postprocess.vert.spv",
                                        "assets/shaders/sun_shafts_composite.frag.spv", "sun shafts");
    if (!shaders) return false;

    // Screened on rather than added: rays + scene * (1 - rays). Over a dark
    // trunk that is nearly the whole ray; over the bright sky around the sun,
    // which the rays come from and so are brightest over, it is a little -
    // added straight on, that sky clipped to a white disc a third of the
    // screen across. The alpha is left alone; the interface drawn after this
    // blends against the colour, and nothing reads the alpha.
    VkPipelineColorBlendAttachmentState add{};
    add.blendEnable = VK_TRUE;
    add.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    add.colorBlendOp = VK_BLEND_OP_ADD;
    add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    add.alphaBlendOp = VK_BLEND_OP_ADD;
    add.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    compositePipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(add)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(compositeLayout_)
        .setRenderPass(ctx_->getOverlayRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, ctx_->getPipelineCache());
    if (compositePipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("SunShafts: failed to create the composite pipeline");
        return false;
    }
    compositeFormat_ = format;
    return true;
}

void SunShafts::destroyTargets() {
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroyScreenTarget(*ctx_, frameCopy_[i]);
        destroyScreenTarget(*ctx_, source_[i]);
        destroyScreenTarget(*ctx_, rays_[i]);
        drawThisFrame_[i] = false;
        // The mask set names the images just destroyed.
        maskSetSerial_[i] = 0;
    }
    targetExtent_ = {.width = 0, .height = 0};
    sourceExtent_ = {.width = 0, .height = 0};
}

bool SunShafts::ensureTargets(VkExtent2D extent) {
    if (extent.width == sourceExtent_.width && extent.height == sourceExtent_.height &&
        rays_[0].image != VK_NULL_HANDLE) {
        return true;
    }
    // Both slots' images are replaced, and the other slot's frame may still be
    // in flight with them bound. A resize is rare enough to wait for it.
    ::wowee::rendering::deviceWaitIdle(ctx_->getDevice());
    destroyTargets();

    // A quarter a side. The rays are blurred to nothing finer than that.
    targetExtent_ = {.width = std::max(1u, extent.width / 4), .height = std::max(1u, extent.height / 4)};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (!createScreenTarget(*ctx_, frameCopy_[i], targetExtent_, kCopyFormat,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !createScreenTarget(*ctx_, source_[i], targetExtent_, kRaysFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !createScreenTarget(*ctx_, rays_[i], targetExtent_, kRaysFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            LOG_WARNING("SunShafts: could not allocate ", targetExtent_.width, "x",
                        targetExtent_.height, " targets - sun shafts off");
            destroyTargets();
            usable_ = false;
            return false;
        }
    }

    // The sky and ray images live in GENERAL, each written by one pass and
    // sampled by the next from the same layout.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b[MAX_FRAMES * 2]{};
        for (uint32_t i = 0; i < MAX_FRAMES * 2; i++) {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = i < MAX_FRAMES ? rays_[i].image : source_[i - MAX_FRAMES].image;
            b[i].subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = MAX_FRAMES * 2;
        dep.pImageMemoryBarriers = b;
        cmdPipelineBarrier2(cmd, dep);
    });

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorImageInfo sourceIn{.sampler = sampler_, .imageView = source_[i].view,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo raysOut{.sampler = VK_NULL_HANDLE, .imageView = rays_[i].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo raysIn{.sampler = sampler_, .imageView = rays_[i].view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3]{};
        for (auto& write : w) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.descriptorCount = 1;
        }
        w[0].dstSet = marchSets_[i];
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &sourceIn;
        w[1].dstSet = marchSets_[i];
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &raysOut;
        w[2].dstSet = compositeSets_[i];
        w[2].dstBinding = 0;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &raysIn;
        vkUpdateDescriptorSets(ctx_->getDevice(), 3, w, 0, nullptr);
    }
    sourceExtent_ = extent;
    LOG_INFO("SunShafts: ", targetExtent_.width, "x", targetExtent_.height, " targets for a ",
             extent.width, "x", extent.height, " swapchain");
    return true;
}

VkImageView SunShafts::depthViewFor(VkImage image, uint64_t generation) {
    if (image == depthImage_ && generation == depthGeneration_ && depthView_ != VK_NULL_HANDLE) return depthView_;
    VkDevice device = ctx_->getDevice();
    if (depthView_ != VK_NULL_HANDLE) {
        // A slot's mask set may still name it in a frame in flight.
        VkImageView old = depthView_;
        ctx_->deferAfterAllFrameFences([device, old]() { vkDestroyImageView(device, old, nullptr); });
        depthView_ = VK_NULL_HANDLE;
    }
    depthImage_ = image;
    depthGeneration_ = generation;
    VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ctx_->getDepthFormat();
    vi.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1,
                           .baseArrayLayer = 0, .layerCount = 1};
    if (vkCreateImageView(device, &vi, nullptr, &depthView_) != VK_SUCCESS) {
        depthView_ = VK_NULL_HANDLE;
        depthImage_ = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    ++depthViewSerial_;
    return depthView_;
}

bool SunShafts::record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                       VkExtent2D extent, const FrameInputs& in) {
    ZoneScopedN("SunShafts::record");
    if (frame >= MAX_FRAMES) return false;
    drawThisFrame_[frame] = false;
    // No depth, no telling the sky from what stands in front of it: nothing.
    if (!usable_ || in.strength <= 0.0f || cmd == VK_NULL_HANDLE || swapchainImage == VK_NULL_HANDLE ||
        in.depthImage == VK_NULL_HANDLE || extent.width < 4 || extent.height < 4) {
        return false;
    }
    if (!ensureTargets(extent) || !ensureCompositePipeline()) return false;
    // The march stores into this slot's ray image through its set. Neither may
    // name nothing: a store through an empty descriptor is a GPU write to
    // address zero, which loses the device instead of a frame's rays.
    if (maskSets_[frame] == VK_NULL_HANDLE || marchSets_[frame] == VK_NULL_HANDLE ||
        compositeSets_[frame] == VK_NULL_HANDLE || rays_[frame].view == VK_NULL_HANDLE ||
        source_[frame].view == VK_NULL_HANDLE || frameCopy_[frame].image == VK_NULL_HANDLE ||
        frameCopy_[frame].view == VK_NULL_HANDLE || marchPipeline_ == VK_NULL_HANDLE ||
        maskPipeline_ == VK_NULL_HANDLE || maskPipelineMs_ == VK_NULL_HANDLE) {
        return false;
    }
    const VkImageView depthView = depthViewFor(in.depthImage, in.depthGeneration);
    if (depthView == VK_NULL_HANDLE) return false;
    // This slot's set was last bound by its frame two frames ago, which has
    // finished: it can be written again.
    if (maskSetSerial_[frame] != depthViewSerial_) {
        VkDescriptorImageInfo copyIn{.sampler = sampler_, .imageView = frameCopy_[frame].view,
                                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depthIn{.sampler = pointSampler_, .imageView = depthView,
                                      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo sourceOut{.sampler = VK_NULL_HANDLE, .imageView = source_[frame].view,
                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo* infos[3] = {&copyIn, &depthIn, &sourceOut};
        VkWriteDescriptorSet w[3]{};
        for (uint32_t b = 0; b < 3; b++) {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = maskSets_[frame];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[b].pImageInfo = infos[b];
        }
        vkUpdateDescriptorSets(ctx_->getDevice(), 3, w, 0, nullptr);
        maskSetSerial_[frame] = depthViewSerial_;
    }

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

    // The finished frame, from wherever its last pass left it - the scene's or
    // the upscaler's, both of which end in PRESENT_SRC - and a water capture
    // may have read it just before.
    barrier(swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    // Its previous contents are not wanted; the march that read them was two
    // frames ago in this slot.
    barrier(frameCopy_[frame].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1};
    blit.dstOffsets[1] = {.x = static_cast<int32_t>(targetExtent_.width),
                          .y = static_cast<int32_t>(targetExtent_.height), .z = 1};
    vkCmdBlitImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   frameCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // Back for the overlay pass, which loads it from PRESENT_SRC.
    barrier(swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(frameCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    // This slot's rays were last sampled by its composite two frames ago, and
    // its sky by the march of that frame.
    barrier(rays_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    barrier(source_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

    // The scene's depth, from wherever its last pass left it, to be sampled -
    // and back afterwards, so whatever comes next finds it as it was.
    const auto depthBarrier = [&](VkImageLayout from, VkImageLayout to, bool toRead) {
        const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        const VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = toRead ? (tests | readers) : readers;
        b.srcAccessMask = toRead ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0;
        b.dstStageMask = toRead ? readers : (tests | readers);
        b.dstAccessMask = toRead ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                 : (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = in.depthImage;
        b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1,
                              .baseArrayLayer = 0, .layerCount = 1};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };
    depthBarrier(in.depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      in.depthMultisampled ? maskPipelineMs_ : maskPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, maskLayout_, 0, 1, &maskSets_[frame], 0, nullptr);
    vkCmdDispatch(cmd, (targetExtent_.width + kMarchGroup - 1) / kMarchGroup,
                  (targetExtent_.height + kMarchGroup - 1) / kMarchGroup, 1);
    depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, in.depthLayout, false);
    barrier(source_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    MarchPush push{};
    push.sun = glm::vec4(in.sunUV, static_cast<float>(extent.width) / static_cast<float>(extent.height),
                         in.strength);
    push.tint = glm::vec4(in.tint, 0.0f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchLayout_, 0, 1, &marchSets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, marchLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (targetExtent_.width + kMarchGroup - 1) / kMarchGroup,
                  (targetExtent_.height + kMarchGroup - 1) / kMarchGroup, 1);

    barrier(rays_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    drawThisFrame_[frame] = true;
    return true;
}

void SunShafts::composite(VkCommandBuffer cmd, uint32_t frame) {
    if (frame >= MAX_FRAMES || !drawThisFrame_[frame] || compositePipeline_ == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositeLayout_, 0, 1,
                            &compositeSets_[frame], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void SunShafts::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    ::wowee::rendering::deviceWaitIdle(device);
    destroyTargets();
    destroy(device, descPool_);
    if (depthView_ != VK_NULL_HANDLE) vkDestroyImageView(device, depthView_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        maskSets_[i] = VK_NULL_HANDLE;
        maskSetSerial_[i] = 0;
        marchSets_[i] = VK_NULL_HANDLE;
        compositeSets_[i] = VK_NULL_HANDLE;
    }
    destroy(device, maskPipeline_);
    destroy(device, maskPipelineMs_);
    destroy(device, marchPipeline_);
    destroy(device, compositePipeline_);
    destroy(device, maskLayout_);
    destroy(device, marchLayout_);
    destroy(device, compositeLayout_);
    destroy(device, maskSetLayout_);
    destroy(device, marchSetLayout_);
    destroy(device, compositeSetLayout_);
    sampler_ = VK_NULL_HANDLE;  // the context's cache owns it
    pointSampler_ = VK_NULL_HANDLE;
    compositeFormat_ = VK_FORMAT_UNDEFINED;
    usable_ = false;
    ctx_ = nullptr;
}

} // namespace rendering
} // namespace wowee
