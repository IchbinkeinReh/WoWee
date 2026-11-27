#include "rendering/volumetric_light.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kGroup = 8;  // 8x8, every pass
constexpr uint32_t kSetBindings = 8;

/// Must match VLParams in volumetric_light.comp.glsl (std140).
struct VLParamsGPU {
    glm::mat4 invViewProj;
    glm::mat4 viewProj;
    glm::vec4 camPos;      // w = how far the sky body's light is gathered
    glm::vec4 camForward;
    glm::vec4 bodyDir;     // w = its share
    glm::vec4 bodyColor;   // w = the forward lobe's share
    glm::vec4 air;         // x, y = scatter per yard (body, lamps), z, w = samples (body, per lamp)
    glm::ivec4 modes;      // x = lamps, y = body occlusion, z = lamp occlusion, w = scene has content
    glm::vec4 screen;      // xy = the march's size, z = the frame's number, w = history usable
    rt_lights::PointLightGPU lights[volumetric_light::kMaxLights];
    glm::mat4 prevViewProj;  // last drawn frame's, for the history (volumetric_light_temporal.comp.glsl)
    glm::vec4 prevCamPos;    // last drawn frame's eye
};
static_assert(sizeof(VLParamsGPU) == 3 * 64 + 8 * 16 + volumetric_light::kMaxLights * 48,
              "VLParamsGPU is uploaded as std140");

/// The composite's push constants (volumetric_light_composite.frag.glsl).
struct CompositePush {
    glm::vec4 keepRect;
    glm::vec4 mode;  // x 1 = the debug view, y its gain
};

}  // namespace

VolumetricLight::~VolumetricLight() {
    shutdown();
}

bool VolumetricLight::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    if (!ctx || perFrameLayout == VK_NULL_HANDLE) return false;
    ctx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    VkDevice device = ctx_->getDevice();

    // Every one of these is required of a Vulkan device, but a device that
    // breaks the rule should cost the option and not the session.
    const auto has = [&](VkFormat format, VkFormatFeatureFlags needed) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), format, &props);
        return (props.optimalTilingFeatures & needed) == needed;
    };
    if (!has(kDepthFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !has(kLightFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
        !has(ctx_->getDepthFormat(), VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        LOG_WARNING("VolumetricLight: its images cannot be stored to or sampled here - volumetric light unavailable");
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
    sampCI.maxLod = 0.0f;
    linearSampler_ = ctx_->getOrCreateSampler(sampCI);
    sampCI.magFilter = VK_FILTER_NEAREST;
    sampCI.minFilter = VK_FILTER_NEAREST;
    pointSampler_ = ctx_->getOrCreateSampler(sampCI);
    if (linearSampler_ == VK_NULL_HANDLE || pointSampler_ == VK_NULL_HANDLE) return false;

    // Set 1, shared by the four compute passes; each reads what it names.
    //   0  VLParams
    //   1  the shadow map (or the 1x1 stand-in)
    //   2  the scene's depth
    //   3  the depth at the march's size
    //   4  the march's light
    //   5  the blurred light
    //   6  the history of the frames before
    //   7  the history with this frame's light in it
    const VkDescriptorType types[kSetBindings] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    };
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < kSetBindings; i++) {
        bindings.push_back({.binding = i, .descriptorType = types[i], .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .pImmutableSamplers = nullptr});
    }
    setLayout_ = createDescriptorSetLayout(device, bindings);
    VkDescriptorSetLayoutBinding light{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                       .pImmutableSamplers = nullptr};
    compositeSetLayout_ = createDescriptorSetLayout(device, {light});
    if (setLayout_ == VK_NULL_HANDLE || compositeSetLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("VolumetricLight: failed to create descriptor set layouts");
        return false;
    }
    baseLayout_ = createPipelineLayout(device, {perFrameLayout_, setLayout_});
    const VkPushConstantRange keepRange{.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0,
                                        .size = sizeof(CompositePush)};
    compositeLayout_ = createPipelineLayout(device, {compositeSetLayout_}, {keepRange});
    if (baseLayout_ == VK_NULL_HANDLE || compositeLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("VolumetricLight: failed to create pipeline layouts");
        return false;
    }

    const auto computePipeline = [&](const char* path, VkPipelineLayout layout, VkPipeline& out) {
        VkShaderModule module;
        if (!module.loadFromFile(device, path)) {
            LOG_ERROR("VolumetricLight: failed to load ", path);
            return false;
        }
        VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = layout;
        const bool ok = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI,
                                                 nullptr, &out) == VK_SUCCESS;
        module.destroy();
        if (!ok) LOG_ERROR("VolumetricLight: failed to create the pipeline of ", path);
        return ok;
    };
    if (!computePipeline("assets/shaders/volumetric_light_depth.comp.spv", baseLayout_, depthPipeline_) ||
        !computePipeline("assets/shaders/volumetric_light_depth_ms.comp.spv", baseLayout_, depthPipelineMs_) ||
        !computePipeline("assets/shaders/volumetric_light.comp.spv", baseLayout_, marchPipeline_) ||
        !computePipeline("assets/shaders/volumetric_light_blur.comp.spv", baseLayout_, blurPipeline_) ||
        !computePipeline("assets/shaders/volumetric_light_temporal.comp.spv", baseLayout_, temporalPipeline_)) {
        return false;
    }

    VkDescriptorPoolSize sizes[3] = {
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_FRAMES},
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES * 3},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = MAX_FRAMES * 5},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES * 2;
    poolCI.poolSizeCount = 3;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("VolumetricLight: failed to create descriptor pool");
        return false;
    }
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descPool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &setLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &sets_[i]) != VK_SUCCESS) return false;
        alloc.pSetLayouts = &compositeSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &compositeSets_[i]) != VK_SUCCESS) return false;

        VkBufferCreateInfo bufCI{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufCI.size = sizeof(VLParamsGPU);
        bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(ctx_->getAllocator(), &bufCI, &allocCI, &paramsUBO_[i], &paramsAlloc_[i],
                            &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("VolumetricLight: failed to create parameter buffer ", i);
            return false;
        }
        paramsMapped_[i] = mapInfo.pMappedData;
    }

    // The stand-in for a shadow map that is not readable this frame (shadows
    // off, before the first world frame): bound so the set is whole, never
    // read - the sky body then has no occlusion and is left out.
    if (!createScreenTarget(*ctx_, dummyShadow_, {.width = 1, .height = 1}, kDepthFormat,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOG_ERROR("VolumetricLight: failed to create the stand-in shadow map");
        return false;
    }
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dummyShadow_.image;
        b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                              .baseArrayLayer = 0, .layerCount = 1};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
        VkClearColorValue clear{};
        vkCmdClearColorImage(cmd, dummyShadow_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                             &b.subresourceRange);
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cmdPipelineBarrier2(cmd, dep);
    });

    // Now rather than at the first frame with the option on, so a pipeline
    // that will not build says so at start-up.
    if (!ensureCompositePipeline()) return false;

    usable_ = true;
    return true;
}

bool VolumetricLight::ensureCompositePipeline() {
    const VkFormat format = ctx_->getSwapchainFormat();
    if (compositePipeline_ != VK_NULL_HANDLE && debugPipeline_ != VK_NULL_HANDLE && format == compositeFormat_) {
        return true;
    }
    VkDevice device = ctx_->getDevice();
    if (compositePipeline_ != VK_NULL_HANDLE || debugPipeline_ != VK_NULL_HANDLE) {
        ::wowee::rendering::deviceWaitIdle(device);
    }
    destroy(device, compositePipeline_);
    destroy(device, debugPipeline_);

    ShaderPair shaders = loadShaderPair(device, "assets/shaders/postprocess.vert.spv",
                                        "assets/shaders/volumetric_light_composite.frag.spv", "volumetric light");
    if (!shaders) return false;

    // Screened on, as the sun shafts are: light + frame * (1 - light), so a
    // glow over the bright sky never clips it to white. The alpha is left
    // alone.
    VkPipelineColorBlendAttachmentState screen{};
    screen.blendEnable = VK_TRUE;
    screen.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    screen.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    screen.colorBlendOp = VK_BLEND_OP_ADD;
    screen.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    screen.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    screen.alphaBlendOp = VK_BLEND_OP_ADD;
    screen.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    compositePipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(screen)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(compositeLayout_)
        .setRenderPass(ctx_->getOverlayRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, ctx_->getPipelineCache());
    // The debug view "Volumetric light only" (dbgview, Renderer::kVolumetricLightOnlyView): the light over black, written
    // over the picture without a blend.
    VkPipelineColorBlendAttachmentState over{};
    over.blendEnable = VK_FALSE;
    over.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    debugPipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(over)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(compositeLayout_)
        .setRenderPass(ctx_->getOverlayRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, ctx_->getPipelineCache());
    if (compositePipeline_ == VK_NULL_HANDLE || debugPipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("VolumetricLight: failed to create the composite pipelines");
        return false;
    }
    compositeFormat_ = format;
    return true;
}

VkPipeline VolumetricLight::tracedPipeline(VkDescriptorSetLayout sceneLayout, bool hardware) {
    if (sceneLayout == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkDevice device = ctx_->getDevice();
    if (sceneLayout != tracedSceneLayout_) {
        // The scene made its set layout again: what was built on the old one
        // goes, once no frame can still be using it.
        if (tracedLayout_ != VK_NULL_HANDLE) ::wowee::rendering::deviceWaitIdle(device);
        destroyTracedPipelines();
        tracedSceneLayout_ = sceneLayout;
        tracedLayout_ = createPipelineLayout(device, {perFrameLayout_, setLayout_, sceneLayout});
        if (tracedLayout_ == VK_NULL_HANDLE) {
            LOG_ERROR("VolumetricLight: failed to create the traced pipeline layout");
            tracedSwFailed_ = tracedHwFailed_ = true;
            return VK_NULL_HANDLE;
        }
    }
    VkPipeline& out = hardware ? tracedHw_ : tracedSw_;
    bool& failed = hardware ? tracedHwFailed_ : tracedSwFailed_;
    if (out != VK_NULL_HANDLE || failed) return out;
    // Only on a device whose ray queries are on: a module that declares them
    // cannot even be made elsewhere (MoltenVK has none).
    if (hardware && !ctx_->hardwareRayQueryEnabled()) {
        failed = true;
        return VK_NULL_HANDLE;
    }
    const char* path = hardware ? "assets/shaders/volumetric_light_hw.comp.spv"
                                : "assets/shaders/volumetric_light_sw.comp.spv";
    VkShaderModule module;
    if (!module.loadFromFile(device, path)) {
        LOG_WARNING("VolumetricLight: failed to load ", path, " - its light is not traced");
        failed = true;
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    cpCI.layout = tracedLayout_;
    if (vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI, nullptr, &out) != VK_SUCCESS) {
        LOG_WARNING("VolumetricLight: the traced march of ", path, " did not build - its light is not traced");
        out = VK_NULL_HANDLE;
        failed = true;
    }
    module.destroy();
    return out;
}

void VolumetricLight::destroyTracedPipelines() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    destroy(device, tracedSw_);
    destroy(device, tracedHw_);
    destroy(device, tracedLayout_);
    tracedSceneLayout_ = VK_NULL_HANDLE;
    tracedSwFailed_ = tracedHwFailed_ = false;
}

void VolumetricLight::destroyTargets() {
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroyScreenTarget(*ctx_, lowDepth_[i]);
        destroyScreenTarget(*ctx_, march_[i]);
        destroyScreenTarget(*ctx_, blurred_[i]);
        destroyScreenTarget(*ctx_, history_[i]);
        drawThisFrame_[i] = false;
    }
    historyValid_ = false;
    targetExtent_ = {.width = 0, .height = 0};
    sourceExtent_ = {.width = 0, .height = 0};
    targetQuality_ = Quality::Off;
}

void VolumetricLight::setQuality(Quality q) {
    if (q == quality_) return;
    quality_ = q;
    if (q == Quality::Off && ctx_ && lowDepth_[0].image != VK_NULL_HANDLE) {
        // Its images go with the option: either slot's may still be in flight.
        ::wowee::rendering::deviceWaitIdle(ctx_->getDevice());
        destroyTargets();
        LOG_INFO("VolumetricLight: off");
    }
}

bool VolumetricLight::ensureTargets(VkExtent2D extent) {
    if (extent.width == sourceExtent_.width && extent.height == sourceExtent_.height &&
        targetQuality_ == quality_ && blurred_[0].image != VK_NULL_HANDLE) {
        return true;
    }
    ::wowee::rendering::deviceWaitIdle(ctx_->getDevice());
    destroyTargets();

    const unsigned divisor = std::max(volumetric_light::budgetFor(quality_).divisor, 1u);
    targetExtent_ = {.width = std::max(1u, extent.width / divisor), .height = std::max(1u, extent.height / divisor)};
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (!createScreenTarget(*ctx_, lowDepth_[i], targetExtent_, kDepthFormat, usage) ||
            !createScreenTarget(*ctx_, march_[i], targetExtent_, kLightFormat, usage) ||
            !createScreenTarget(*ctx_, blurred_[i], targetExtent_, kLightFormat, usage) ||
            !createScreenTarget(*ctx_, history_[i], targetExtent_, kLightFormat, usage)) {
            LOG_WARNING("VolumetricLight: could not allocate ", targetExtent_.width, "x", targetExtent_.height,
                        " targets - volumetric light off this frame");
            destroyTargets();
            return false;
        }
    }

    // All four live in GENERAL, each written by one pass and read by the next.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b[MAX_FRAMES * 4]{};
        for (uint32_t i = 0; i < MAX_FRAMES * 4; i++) {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            const uint32_t slot = i % MAX_FRAMES;
            b[i].image = i < MAX_FRAMES       ? lowDepth_[slot].image
                         : i < MAX_FRAMES * 2 ? march_[slot].image
                         : i < MAX_FRAMES * 3 ? blurred_[slot].image
                                              : history_[slot].image;
            b[i].subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                                     .baseArrayLayer = 0, .layerCount = 1};
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = MAX_FRAMES * 4;
        dep.pImageMemoryBarriers = b;
        cmdPipelineBarrier2(cmd, dep);
    });

    // New targets hold no history: the next frame starts afresh.
    historyValid_ = false;
    sourceExtent_ = extent;
    targetQuality_ = quality_;
    LOG_INFO("VolumetricLight: ", targetExtent_.width, "x", targetExtent_.height, " targets for a ",
             extent.width, "x", extent.height, " swapchain");
    return true;
}

bool VolumetricLight::record(VkCommandBuffer cmd, uint32_t frame, VkExtent2D extent, const FrameInputs& in) {
    ZoneScopedN("VolumetricLight::record");
    if (frame >= MAX_FRAMES) return false;
    drawThisFrame_[frame] = false;
    // A frame without the light breaks the history: the next starts afresh
    // (volumetric_light::historyUsable).
    const bool lastFrameDrew = historyValid_;
    historyValid_ = false;
    if (!usable_ || quality_ == Quality::Off || !in.draw || cmd == VK_NULL_HANDLE ||
        in.depthImage == VK_NULL_HANDLE || in.perFrameSet == VK_NULL_HANDLE || extent.width < 8 ||
        extent.height < 8) {
        return false;
    }
    // ensureTargets drops the history when it makes new targets.
    historyValid_ = lastFrameDrew;
    const bool targetsReady = ensureTargets(extent);
    const bool sameTargets = historyValid_;
    historyValid_ = false;
    if (!targetsReady || !ensureCompositePipeline()) return false;
    const uint32_t histRead = historyWrite_;
    const uint32_t histWrite = historyWrite_ ^ 1u;
    if (sets_[frame] == VK_NULL_HANDLE || compositeSets_[frame] == VK_NULL_HANDLE ||
        lowDepth_[frame].view == VK_NULL_HANDLE || march_[frame].view == VK_NULL_HANDLE ||
        blurred_[frame].view == VK_NULL_HANDLE || history_[histRead].view == VK_NULL_HANDLE ||
        history_[histWrite].view == VK_NULL_HANDLE || paramsMapped_[frame] == nullptr) {
        return false;
    }
    const VkImageView depthView = depthView_.viewFor(*ctx_, in.depthImage, in.depthGeneration);
    if (depthView == VK_NULL_HANDLE) return false;
    // The history only from a frame that drew the light into these targets,
    // with the camera not moved past a cut (a teleport, a load).
    const bool historyUsable = volumetric_light::historyUsable(
        lastFrameDrew, sameTargets, glm::length(in.cameraPos - prevCamPos_));

    // Which march: the traced one only when the scene can be bound and its
    // pipeline built; otherwise the plain one, the traced occlusions falling
    // back to the shadow map and the depth.
    const bool wantsTrace = in.bodyOcclusion == Occlusion::Traced || in.lampOcclusion == Occlusion::Traced;
    VkPipeline march = marchPipeline_;
    VkPipelineLayout marchLayout = baseLayout_;
    bool traced = false;
    if (wantsTrace && in.sceneSet != VK_NULL_HANDLE) {
        if (VkPipeline p = tracedPipeline(in.sceneLayout, in.hardware)) {
            march = p;
            marchLayout = tracedLayout_;
            traced = true;
        }
    }
    const volumetric_light::Budget budget = volumetric_light::budgetFor(quality_);
    Occlusion body = in.bodyOcclusion;
    if (body == Occlusion::Traced && !traced) body = in.shadowView != VK_NULL_HANDLE ? Occlusion::ShadowMap : Occlusion::None;
    if (body == Occlusion::ShadowMap && in.shadowView == VK_NULL_HANDLE) body = Occlusion::None;
    Occlusion lamps = in.lampOcclusion;
    if (lamps == Occlusion::Traced && !traced) lamps = Occlusion::DepthSteps;

    // This slot's buffer and set were last used by its frame two frames ago,
    // which has finished.
    VLParamsGPU params{};
    const glm::mat4 viewProj = in.projection * in.view;
    params.invViewProj = glm::inverse(viewProj);
    params.viewProj = viewProj;
    params.camPos = glm::vec4(in.cameraPos, volumetric_light::kSunReach);
    // The view's forward axis: the third row of the view's rotation, negated.
    params.camForward = glm::vec4(-in.view[0][2], -in.view[1][2], -in.view[2][2], 0.0f);
    params.bodyDir = glm::vec4(in.bodyDir, body == Occlusion::None ? 0.0f : std::max(in.bodyShare, 0.0f));
    params.bodyColor = glm::vec4(in.bodyColor, in.lobe);
    // The compute tracer walks its tree per ray: its lamps take the lower
    // quality's samples whatever the setting.
    const unsigned lampSteps = (lamps == Occlusion::Traced && !in.hardware)
                                   ? std::min(budget.lightSteps, volumetric_light::budgetFor(Quality::Low).lightSteps)
                                   : budget.lightSteps;
    params.air = glm::vec4(in.bodyScatter, in.lampScatter, static_cast<float>(budget.sunSteps),
                           static_cast<float>(lampSteps));
    const size_t lampCount = std::min<size_t>(in.lamps.size(), std::min(budget.maxLights, volumetric_light::kMaxLights));
    for (size_t i = 0; i < lampCount; ++i) params.lights[i] = in.lamps[i];
    params.modes = glm::ivec4(static_cast<int>(lampCount), static_cast<int>(body), static_cast<int>(lamps),
                              in.sceneHasContent ? 1 : 0);
    params.screen = glm::vec4(static_cast<float>(targetExtent_.width), static_cast<float>(targetExtent_.height),
                              static_cast<float>(frameNumber_), historyUsable ? 1.0f : 0.0f);
    params.prevViewProj = prevViewProj_;
    params.prevCamPos = glm::vec4(prevCamPos_, 0.0f);
    std::memcpy(paramsMapped_[frame], &params, sizeof(params));

    {
        VkDescriptorBufferInfo ubo{.buffer = paramsUBO_[frame], .offset = 0, .range = sizeof(VLParamsGPU)};
        VkDescriptorImageInfo shadow{.sampler = pointSampler_,
                                     .imageView = in.shadowView != VK_NULL_HANDLE ? in.shadowView : dummyShadow_.view,
                                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depth{.sampler = pointSampler_, .imageView = depthView,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo low{.sampler = VK_NULL_HANDLE, .imageView = lowDepth_[frame].view,
                                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo marchImg{.sampler = VK_NULL_HANDLE, .imageView = march_[frame].view,
                                       .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo blurImg{.sampler = VK_NULL_HANDLE, .imageView = blurred_[frame].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo histIn{.sampler = VK_NULL_HANDLE, .imageView = history_[histRead].view,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo histOut{.sampler = VK_NULL_HANDLE, .imageView = history_[histWrite].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo* images[kSetBindings] = {nullptr, &shadow, &depth,  &low,
                                                             &marchImg, &blurImg, &histIn, &histOut};
        VkWriteDescriptorSet w[kSetBindings + 1]{};
        for (uint32_t b = 0; b < kSetBindings; b++) {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = sets_[frame];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                  : b <= 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                           : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (b == 0) w[b].pBufferInfo = &ubo;
            else w[b].pImageInfo = images[b];
        }
        // The composite reads the history this frame writes. This slot's set
        // was last read by its frame two frames ago, which has finished.
        VkDescriptorImageInfo lightIn{.sampler = linearSampler_, .imageView = history_[histWrite].view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet& c = w[kSetBindings];
        c.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        c.dstSet = compositeSets_[frame];
        c.dstBinding = 0;
        c.descriptorCount = 1;
        c.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        c.pImageInfo = &lightIn;
        vkUpdateDescriptorSets(ctx_->getDevice(), kSetBindings + 1, w, 0, nullptr);
    }

    const auto barrier = [&](VkImage image, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                              .baseArrayLayer = 0, .layerCount = 1};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };
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
    const uint32_t gx = (targetExtent_.width + kGroup - 1) / kGroup;
    const uint32_t gy = (targetExtent_.height + kGroup - 1) / kGroup;
    const VkDescriptorSet baseSets[2] = {in.perFrameSet, sets_[frame]};

    // The depth at the march's size. This slot's images were last read by
    // its passes and its composite two frames ago.
    barrier(lowDepth_[frame].image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    depthBarrier(in.depthLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, in.depthMultisampled ? depthPipelineMs_ : depthPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, baseLayout_, 0, 2, baseSets, 0, nullptr);
    vkCmdDispatch(cmd, gx, gy, 1);
    depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, in.depthLayout, false);
    barrier(lowDepth_[frame].image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // The march.
    barrier(march_[frame].image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchLayout, 0, 2, baseSets, 0, nullptr);
    if (traced) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, marchLayout, 2, 1, &in.sceneSet, 0, nullptr);
    }
    vkCmdDispatch(cmd, gx, gy, 1);
    barrier(march_[frame].image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // The blur.
    barrier(blurred_[frame].image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, baseLayout_, 0, 2, baseSets, 0, nullptr);
    vkCmdDispatch(cmd, gx, gy, 1);
    barrier(blurred_[frame].image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // The history (volumetric_light_temporal.comp.glsl). The one it writes
    // was last read by the composite two frames ago and by the last frame's
    // history pass; the one it reads was written by the last frame's, whose
    // barrier made it readable here.
    const VkPipelineStageFlags2 histReaders = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier(history_[histWrite].image, histReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporalPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, baseLayout_, 0, 2, baseSets, 0, nullptr);
    vkCmdDispatch(cmd, gx, gy, 1);
    barrier(history_[histWrite].image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            histReaders, VK_ACCESS_2_SHADER_READ_BIT);

    historyWrite_ = histWrite;
    historyValid_ = true;
    prevViewProj_ = viewProj;
    prevCamPos_ = in.cameraPos;
    frameNumber_ = (frameNumber_ + 1u) % 1024u;
    drawThisFrame_[frame] = true;
    keepRect_[frame] = in.keepRect;
    debugView_[frame] = in.debugView;
    return true;
}

void VolumetricLight::composite(VkCommandBuffer cmd, uint32_t frame) {
    if (frame >= MAX_FRAMES || !drawThisFrame_[frame]) return;
    const bool debug = debugView_[frame];
    const VkPipeline pipeline = debug ? debugPipeline_ : compositePipeline_;
    if (pipeline == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositeLayout_, 0, 1, &compositeSets_[frame], 0,
                            nullptr);
    const CompositePush push{.keepRect = keepRect_[frame],
                             .mode = glm::vec4(debug ? 1.0f : 0.0f, volumetric_light::kDebugViewGain, 0.0f, 0.0f)};
    vkCmdPushConstants(cmd, compositeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

bool VolumetricLight::debugViewDrawn(uint32_t frame) const {
    return frame < MAX_FRAMES && drawThisFrame_[frame] && debugView_[frame];
}

void VolumetricLight::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    ::wowee::rendering::deviceWaitIdle(device);
    destroyTargets();
    destroyScreenTarget(*ctx_, dummyShadow_);
    destroyTracedPipelines();
    destroy(device, descPool_);
    depthView_.destroy(device);
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        sets_[i] = VK_NULL_HANDLE;
        compositeSets_[i] = VK_NULL_HANDLE;
        if (paramsUBO_[i] != VK_NULL_HANDLE) destroy(ctx_->getAllocator(), paramsUBO_[i], paramsAlloc_[i]);
        paramsMapped_[i] = nullptr;
    }
    destroy(device, depthPipeline_);
    destroy(device, depthPipelineMs_);
    destroy(device, marchPipeline_);
    destroy(device, blurPipeline_);
    destroy(device, temporalPipeline_);
    destroy(device, compositePipeline_);
    destroy(device, debugPipeline_);
    destroy(device, baseLayout_);
    destroy(device, compositeLayout_);
    destroy(device, setLayout_);
    destroy(device, compositeSetLayout_);
    linearSampler_ = VK_NULL_HANDLE;  // the context's cache owns them
    pointSampler_ = VK_NULL_HANDLE;
    compositeFormat_ = VK_FORMAT_UNDEFINED;
    usable_ = false;
    ctx_ = nullptr;
}

} // namespace rendering
} // namespace wowee
