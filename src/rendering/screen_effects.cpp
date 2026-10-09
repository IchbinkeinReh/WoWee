#include "rendering/screen_effects.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include "rendering/screen_effect_state.hpp"
#include <glm/gtc/packing.hpp>
#include <algorithm>

namespace wowee {
namespace rendering {

namespace {

constexpr VkFormat kCopyFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kGlowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBlurGroup = 8;  // 8x8, screen_glow.comp.glsl
constexpr uint32_t kBlurPasses = 3;  // FFXBox4, FFXGauss4 across, FFXGauss4 down
constexpr uint32_t kCompositeBindings = 5;

/// Must match Push in screen_effects_composite.frag.glsl.
struct CompositePush {
    glm::vec4 params;    // x = glow w, y = blend z, z = mode, w = fog target
    glm::vec4 keepRect;  // the minimap, uv x, y, w, h
    glm::vec4 waveRow;
    glm::vec4 waveMove;
    glm::vec4 extra;
};

/// Must match Push in screen_nether.comp.glsl.
struct NetherPush {
    glm::vec2 step;
    float angle;
    float pad;
    uint32_t packed[18];
};
static_assert(sizeof(NetherPush) <= 128);

/// Must match Push in screen_fog.comp.glsl.
struct FogPush {
    glm::vec4 colour;
    float decay;
    int32_t noiseRow;
    int32_t seedRow;
    float pad;
};

uint32_t packHalf2(float a, float b) {
    return glm::packHalf2x16(glm::vec2(a, b));
}

}  // namespace

ScreenEffects::~ScreenEffects() {
    shutdown();
}

bool ScreenEffects::createComputePipeline(const char* path, VkPipelineLayout layout, VkPipeline& out) {
    VkDevice device = ctx_->getDevice();
    VkShaderModule module;
    if (!module.loadFromFile(device, path)) {
        LOG_ERROR("ScreenEffects: failed to load ", path);
        return false;
    }
    VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    cpCI.layout = layout;
    const bool ok = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI,
                                             nullptr, &out) == VK_SUCCESS;
    module.destroy();
    if (!ok) LOG_ERROR("ScreenEffects: failed to create the pipeline for ", path);
    return ok;
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
        !has(kCopyFormat, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                              VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !has(kGlowFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_WARNING("ScreenEffects: the swapchain cannot be copied or the glow stored here"
                    " - the full-screen effects are unavailable");
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

    // The wave (0x008c2920) and the fog's seed noise (0x007e8e40), made once.
    {
        const std::vector<uint8_t> wave = screen_effect::waveTexture();
        const std::vector<uint8_t> noise = screen_effect::fogNoiseTexture();
        if (!wave_.upload(*ctx_, wave.data(), 128, 128, VK_FORMAT_R8G8B8A8_SNORM, false) ||
            !wave_.createSampler(device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                 VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f) ||
            !fogNoise_.upload(*ctx_, noise.data(), screen_effect::kFogNoiseSize,
                              screen_effect::kFogNoiseSize, VK_FORMAT_R8G8B8A8_UNORM, false) ||
            !fogNoise_.createSampler(device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                     VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f)) {
            LOG_ERROR("ScreenEffects: failed to make the wave and noise textures");
            return false;
        }
    }

    const auto binding = [](uint32_t b, VkDescriptorType type, VkShaderStageFlags stages) {
        return VkDescriptorSetLayoutBinding{.binding = b, .descriptorType = type, .descriptorCount = 1,
                                            .stageFlags = stages, .pImmutableSamplers = nullptr};
    };
    blurSetLayout_ = createDescriptorSetLayout(device, {
        binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
        binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)});
    fogSetLayout_ = createDescriptorSetLayout(device, {
        binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT),
        binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT),
        binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)});
    std::vector<VkDescriptorSetLayoutBinding> comp;
    for (uint32_t b = 0; b < kCompositeBindings; ++b) {
        comp.push_back(binding(b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT));
    }
    compositeSetLayout_ = createDescriptorSetLayout(device, comp);
    if (blurSetLayout_ == VK_NULL_HANDLE || fogSetLayout_ == VK_NULL_HANDLE ||
        compositeSetLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("ScreenEffects: failed to create descriptor set layouts");
        return false;
    }

    const auto pushRange = [](VkShaderStageFlags stages, uint32_t size) {
        return VkPushConstantRange{.stageFlags = stages, .offset = 0, .size = size};
    };
    blurLayout_ = createPipelineLayout(device, {blurSetLayout_},
                                       {pushRange(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(int32_t))});
    netherLayout_ = createPipelineLayout(device, {blurSetLayout_},
                                         {pushRange(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(NetherPush))});
    fogLayout_ = createPipelineLayout(device, {fogSetLayout_},
                                      {pushRange(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(FogPush))});
    compositeLayout_ = createPipelineLayout(device, {compositeSetLayout_},
                                            {pushRange(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(CompositePush))});
    if (blurLayout_ == VK_NULL_HANDLE || netherLayout_ == VK_NULL_HANDLE || fogLayout_ == VK_NULL_HANDLE ||
        compositeLayout_ == VK_NULL_HANDLE) {
        LOG_ERROR("ScreenEffects: failed to create pipeline layouts");
        return false;
    }

    if (!createComputePipeline("assets/shaders/screen_glow.comp.spv", blurLayout_, blurPipeline_) ||
        !createComputePipeline("assets/shaders/screen_nether.comp.spv", netherLayout_, netherPipeline_) ||
        !createComputePipeline("assets/shaders/screen_fog.comp.spv", fogLayout_, fogPipeline_)) {
        return false;
    }

    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = MAX_FRAMES * (kBlurSets + kCompositeBindings) + 2},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = MAX_FRAMES * kBlurSets + 4},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES * (kBlurSets + 1) + 2;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("ScreenEffects: failed to create descriptor pool");
        return false;
    }
    VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descPool_;
    alloc.descriptorSetCount = 1;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        alloc.pSetLayouts = &blurSetLayout_;
        for (uint32_t p = 0; p < kBlurSets; ++p) {
            if (vkAllocateDescriptorSets(device, &alloc, &blurSets_[i][p]) != VK_SUCCESS) return false;
        }
        alloc.pSetLayouts = &compositeSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &compositeSets_[i]) != VK_SUCCESS) return false;
    }
    alloc.pSetLayouts = &fogSetLayout_;
    for (auto& set : fogSets_) {
        if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS) return false;
    }

    if (!ensureFogTargets() || !ensureCompositePipeline()) return false;

    usable_ = true;
    return true;
}

bool ScreenEffects::ensureFogTargets() {
    if (fog_[0].image != VK_NULL_HANDLE) return true;
    const VkExtent2D size{.width = screen_effect::kFogWidth, .height = screen_effect::kFogHeight};
    for (auto& t : fog_) {
        if (!createScreenTarget(*ctx_, t, size, kCopyFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            LOG_ERROR("ScreenEffects: failed to create the fog targets");
            return false;
        }
    }
    // Empty and in GENERAL from the start: written by the fog's step, read
    // by the next step and by the composite from the same layout.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b[2]{};
        for (uint32_t i = 0; i < 2; i++) {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = fog_[i].image;
            b[i].subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = b;
        cmdPipelineBarrier2(cmd, dep);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        for (auto& t : fog_) {
            vkCmdClearColorImage(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        for (uint32_t i = 0; i < 2; i++) {
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        cmdPipelineBarrier2(cmd, dep);
    });

    const VkDescriptorImageInfo noise = fogNoise_.descriptorInfo();
    for (uint32_t i = 0; i < 2; i++) {
        const VkDescriptorImageInfo prev{.sampler = VK_NULL_HANDLE, .imageView = fog_[i].view,
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo next{.sampler = VK_NULL_HANDLE, .imageView = fog_[1 - i].view,
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3]{};
        const VkDescriptorImageInfo* infos[3] = {&prev, &next, &noise};
        const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        for (uint32_t b = 0; b < 3; ++b) {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = fogSets_[i];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = types[b];
            w[b].pImageInfo = infos[b];
        }
        vkUpdateDescriptorSets(ctx_->getDevice(), 3, w, 0, nullptr);
    }
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
        destroyScreenTarget(*ctx_, glowTemp_[i]);
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
            !createScreenTarget(*ctx_, glowTemp_[i], smallExtent_, kGlowFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !createScreenTarget(*ctx_, glow_[i], smallExtent_, kGlowFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            LOG_WARNING("ScreenEffects: could not allocate targets for ", extent.width, "x",
                        extent.height, " - the full-screen effects off");
            destroyTargets();
            usable_ = false;
            return false;
        }
    }

    // The glow and its temporary live in GENERAL, written by the blur passes
    // and sampled by the next one and the composite from the same layout.
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
            b[i].image = i < MAX_FRAMES ? glow_[i].image : glowTemp_[i - MAX_FRAMES].image;
            b[i].subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        }
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = MAX_FRAMES * 2;
        dep.pImageMemoryBarriers = b;
        cmdPipelineBarrier2(cmd, dep);
    });

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        const VkDescriptorImageInfo frameIn{.sampler = sampler_, .imageView = frameCopy_[i].view,
                                            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo glowIn{.sampler = sampler_, .imageView = glow_[i].view,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo tempIn{.sampler = sampler_, .imageView = glowTemp_[i].view,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo glowOut{.sampler = VK_NULL_HANDLE, .imageView = glow_[i].view,
                                            .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo tempOut{.sampler = VK_NULL_HANDLE, .imageView = glowTemp_[i].view,
                                            .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        // Box: frame -> glow. Across: glow -> temp. Down: temp -> glow. The
        // nether's first streak: frame -> temp (its second is "down"'s).
        const VkDescriptorImageInfo* passIn[kBlurSets] = {&frameIn, &glowIn, &tempIn, &frameIn};
        const VkDescriptorImageInfo* passOut[kBlurSets] = {&glowOut, &tempOut, &glowOut, &tempOut};
        VkWriteDescriptorSet w[kBlurSets * 2]{};
        for (auto& write : w) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.descriptorCount = 1;
        }
        for (uint32_t p = 0; p < kBlurSets; ++p) {
            w[p * 2].dstSet = blurSets_[i][p];
            w[p * 2].dstBinding = 0;
            w[p * 2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[p * 2].pImageInfo = passIn[p];
            w[p * 2 + 1].dstSet = blurSets_[i][p];
            w[p * 2 + 1].dstBinding = 1;
            w[p * 2 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[p * 2 + 1].pImageInfo = passOut[p];
        }
        vkUpdateDescriptorSets(ctx_->getDevice(), kBlurSets * 2, w, 0, nullptr);
    }
    sourceExtent_ = extent;
    writeCompositeSets();
    return true;
}

void ScreenEffects::writeCompositeSets() {
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        const VkDescriptorImageInfo infos[kCompositeBindings] = {
            {.sampler = sampler_, .imageView = frameCopy_[i].view,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.sampler = sampler_, .imageView = glow_[i].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = sampler_, .imageView = fog_[0].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = sampler_, .imageView = fog_[1].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            wave_.descriptorInfo(),
        };
        VkWriteDescriptorSet w[kCompositeBindings]{};
        for (uint32_t b = 0; b < kCompositeBindings; ++b) {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = compositeSets_[i];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[b].pImageInfo = &infos[b];
        }
        vkUpdateDescriptorSets(ctx_->getDevice(), kCompositeBindings, w, 0, nullptr);
    }
}

bool ScreenEffects::record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage,
                           VkExtent2D extent, const FrameInputs& in) {
    ZoneScopedN("ScreenEffects::record");
    if (frame >= MAX_FRAMES) return false;
    drawThisFrame_[frame] = false;
    // The glow with nothing to add and nothing to mix changes no pixel.
    const bool idleGlow = in.mode == Mode::Glow && in.glow <= 0.0f && in.blend <= 0.0f && !in.wave;
    if (!usable_ || idleGlow || cmd == VK_NULL_HANDLE ||
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
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   frameCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    barrier(swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(frameCopy_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);

    const auto dispatchSmall = [&]() {
        vkCmdDispatch(cmd, (smallExtent_.width + kBlurGroup - 1) / kBlurGroup,
                      (smallExtent_.height + kBlurGroup - 1) / kBlurGroup, 1);
    };
    const auto computeToNext = [&](VkImage image, bool last) {
        barrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                last ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
    };

    if (in.mode == Mode::Glow || in.mode == Mode::Death || in.mode == Mode::Nether) {
        // This slot's glow and temp were last read two frames ago.
        barrier(glow_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        barrier(glowTemp_[frame].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    if (in.mode == Mode::Glow || in.mode == Mode::Death) {
        // FFXBox4 into the glow, FFXGauss4 across into the temp and down back
        // into the glow (0x008bfe80's passes).
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurPipeline_);
        Target* written[kBlurPasses] = {&glow_[frame], &glowTemp_[frame], &glow_[frame]};
        for (uint32_t p = 0; p < kBlurPasses; ++p) {
            const int32_t mode = static_cast<int32_t>(p);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurLayout_, 0, 1,
                                    &blurSets_[frame][p], 0, nullptr);
            vkCmdPushConstants(cmd, blurLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode), &mode);
            dispatchSmall();
            computeToNext(written[p]->image, p + 1 == kBlurPasses);
        }
    } else if (in.mode == Mode::Nether) {
        // FFXNetherBlur twice (0x007e9b10): the frame streaked into the temp,
        // the temp streaked into the glow, the step 8 of the frame's texels
        // in uv on both axes.
        NetherPush push{};
        const float step = 8.0f / static_cast<float>(extent.width);
        push.step = glm::vec2(step, step);
        push.angle = in.netherAngle;
        for (uint32_t k = 0; k < 18; ++k) {
            push.packed[k] = packHalf2(in.netherValues[k * 2], in.netherValues[k * 2 + 1]);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, netherPipeline_);
        vkCmdPushConstants(cmd, netherLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, netherLayout_, 0, 1,
                                &blurSets_[frame][3], 0, nullptr);
        dispatchSmall();
        computeToNext(glowTemp_[frame].image, false);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, netherLayout_, 0, 1,
                                &blurSets_[frame][2], 0, nullptr);
        dispatchSmall();
        computeToNext(glow_[frame].image, true);
    } else if (in.mode == Mode::Fog) {
        // One step of the fog (0x007e92a0, 0x007e9080), from the target the
        // last step wrote into the other. Its last reader may be the composite
        // of the frame before.
        const uint32_t from = fogLatest_;
        const uint32_t to = 1 - from;
        barrier(fog_[to].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        barrier(fog_[from].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        FogPush push{};
        push.colour = glm::vec4(in.fogColour, 1.0f);
        push.decay = in.fogDecay;
        push.noiseRow = static_cast<int32_t>(in.fogNoiseRow);
        push.seedRow = static_cast<int32_t>(screen_effect::kFogSeedRow);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fogPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fogLayout_, 0, 1,
                                &fogSets_[from], 0, nullptr);
        vkCmdPushConstants(cmd, fogLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (screen_effect::kFogWidth + kBlurGroup - 1) / kBlurGroup,
                      (screen_effect::kFogHeight + kBlurGroup - 1) / kBlurGroup, 1);
        barrier(fog_[to].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
        fogLatest_ = to;
    }

    inputs_[frame] = in;
    fogSource_[frame] = fogLatest_;
    drawThisFrame_[frame] = true;
    return true;
}

void ScreenEffects::composite(VkCommandBuffer cmd, uint32_t frame) {
    if (frame >= MAX_FRAMES || !drawThisFrame_[frame] || compositePipeline_ == VK_NULL_HANDLE) return;
    const FrameInputs& in = inputs_[frame];
    CompositePush push{};
    push.params = glm::vec4(in.glow, in.blend, static_cast<float>(in.mode),
                            static_cast<float>(fogSource_[frame]));
    push.keepRect = in.keepRect;
    if (in.mode == Mode::Glow && in.wave && sourceExtent_.width > 0 && sourceExtent_.height > 0) {
        const screen_effect::WaveTransform w =
            screen_effect::waveTransform(in.timeMs, sourceExtent_.width, sourceExtent_.height);
        push.waveRow = glm::vec4(w.m00, w.m01, w.m10, w.m11);
        // FFXGlowWave's c0: 3 of the frame's texels a unit of wave.
        push.waveMove = glm::vec4(w.tx, w.ty, 3.0f / static_cast<float>(sourceExtent_.width),
                                  3.0f / static_cast<float>(sourceExtent_.height));
    }
    if (in.mode == Mode::Nether) push.extra = glm::vec4(in.netherFade, 0.0f, 0.0f, 0.0f);
    if (in.mode == Mode::Fog) push.extra = glm::vec4(in.fogDesaturate, in.fogBrighten, 0.0f, 0.0f);
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
    for (auto& t : fog_) destroyScreenTarget(*ctx_, t);
    wave_.destroy(device, ctx_->getAllocator());
    fogNoise_.destroy(device, ctx_->getAllocator());
    destroy(device, descPool_);
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        for (auto& set : blurSets_[i]) set = VK_NULL_HANDLE;
        compositeSets_[i] = VK_NULL_HANDLE;
    }
    for (auto& set : fogSets_) set = VK_NULL_HANDLE;
    destroy(device, blurPipeline_);
    destroy(device, netherPipeline_);
    destroy(device, fogPipeline_);
    destroy(device, compositePipeline_);
    destroy(device, blurLayout_);
    destroy(device, netherLayout_);
    destroy(device, fogLayout_);
    destroy(device, compositeLayout_);
    destroy(device, blurSetLayout_);
    destroy(device, fogSetLayout_);
    destroy(device, compositeSetLayout_);
    sampler_ = VK_NULL_HANDLE;  // the context's cache owns it
    compositeFormat_ = VK_FORMAT_UNDEFINED;
    usable_ = false;
    ctx_ = nullptr;
}

} // namespace rendering
} // namespace wowee
