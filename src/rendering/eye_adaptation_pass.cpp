#include "rendering/eye_adaptation_pass.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace wowee {
namespace rendering {

namespace {

/// The part of the screen measured, as fractions of it: the middle, where
/// the world is - clear of the unit frames and the minimap above, the chat
/// and the action bars below.
constexpr float kRegionLeft = 0.2f, kRegionRight = 0.8f;
constexpr float kRegionTop = 0.1f, kRegionBottom = 0.75f;

/// Below this the exposure is taken as one and nothing is drawn.
constexpr float kNeutral = 0.004f;

bool isBgra(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

bool isEightBit(VkFormat format) {
    return isBgra(format) || format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
}

}  // namespace

EyeAdaptationPass::~EyeAdaptationPass() {
    shutdown();
}

bool EyeAdaptationPass::initialize(VkContext* ctx) {
    if (!ctx) return false;
    ctx_ = ctx;
    VkDevice device = ctx_->getDevice();

    // The small copy is the swapchain's own format, so its bytes are the
    // picture's as displayed whatever the encoding, and the CPU knows how to
    // read four bytes a pixel.
    const VkFormat format = ctx_->getSwapchainFormat();
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), format, &props);
    const VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (!isEightBit(format) || (props.optimalTilingFeatures & needed) != needed) {
        LOG_WARNING("EyeAdaptation: the swapchain's format cannot be copied down and read here"
                    " - no eye adaptation");
        return false;
    }
    bgra_ = isBgra(format);

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (!createScreenTarget(*ctx_, small_[i], {.width = kWidth, .height = kHeight}, format,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            LOG_WARNING("EyeAdaptation: could not allocate its copy - no eye adaptation");
            return false;
        }
        VkBufferCreateInfo bufCI{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufCI.size = VkDeviceSize(kWidth) * kHeight * 4;
        bufCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
        allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(ctx_->getAllocator(), &bufCI, &allocCI, &readback_[i].buffer,
                            &readback_[i].allocation, &readback_[i].info) != VK_SUCCESS ||
            readback_[i].info.pMappedData == nullptr) {
            LOG_WARNING("EyeAdaptation: could not allocate its readback - no eye adaptation");
            return false;
        }
    }

    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0,
                                   .size = sizeof(float)};
    compositeLayout_ = createPipelineLayout(device, {}, {push});
    if (compositeLayout_ == VK_NULL_HANDLE || !ensureCompositePipeline()) return false;

    log_ = std::getenv("WOWEE_EXPOSURE_LOG") != nullptr;
    usable_ = true;
    return true;
}

bool EyeAdaptationPass::ensureCompositePipeline() {
    const VkFormat format = ctx_->getSwapchainFormat();
    if (compositePipeline_ != VK_NULL_HANDLE && format == compositeFormat_) return true;
    VkDevice device = ctx_->getDevice();
    // A rebuild replaces a pipeline an in-flight frame may have bound.
    if (compositePipeline_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
    destroy(device, compositePipeline_);

    ShaderPair shaders = loadShaderPair(device, "assets/shaders/postprocess.vert.spv",
                                        "assets/shaders/eye_adaptation.frag.spv", "eye adaptation");
    if (!shaders) return false;

    // A multiply: src x dst + dst x src, the shader putting out half the
    // exposure. The alpha is left alone.
    VkPipelineColorBlendAttachmentState mul{};
    mul.blendEnable = VK_TRUE;
    mul.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mul.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    mul.colorBlendOp = VK_BLEND_OP_ADD;
    mul.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mul.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    mul.alphaBlendOp = VK_BLEND_OP_ADD;
    mul.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    compositePipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(mul)
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(compositeLayout_)
        .setRenderPass(ctx_->getOverlayRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, ctx_->getPipelineCache());
    if (compositePipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("EyeAdaptation: failed to create the composite pipeline");
        return false;
    }
    compositeFormat_ = format;
    return true;
}

void EyeAdaptationPass::forget() {
    adapting_ = false;
    exposure_ = 1.0f;
    for (bool& m : measured_) m = false;
}

void EyeAdaptationPass::record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage, VkExtent2D extent,
                               bool active, float dt) {
    ZoneScopedN("EyeAdaptation::record");
    if (frame >= MAX_FRAMES) return;
    if (!usable_ || !active || cmd == VK_NULL_HANDLE || swapchainImage == VK_NULL_HANDLE || extent.width < 16 ||
        extent.height < 16) {
        forget();
        return;
    }
    // The swapchain may have been rebuilt in another format. The composite
    // is built against it again; the copy keeps the format it was made in,
    // which the blit converts to and the measurement reads.
    if (!ensureCompositePipeline()) {
        forget();
        return;
    }

    // What this slot measured two frames ago. Its fence has been waited on
    // for this frame to be recorded into it, so the copy has landed.
    if (measured_[frame]) {
        vmaInvalidateAllocation(ctx_->getAllocator(), readback_[frame].allocation, 0, VK_WHOLE_SIZE);
        const float measured = eye_adaptation::measureLog2(
            static_cast<const uint8_t*>(readback_[frame].info.pMappedData), kWidth, kHeight, bgra_);
        if (!adapting_) {
            adaptedLog2_ = eye_adaptation::initialAdaptation(measured, params_);
            adapting_ = true;
        }
        // A hitch - a loading stall, a dragged window - is not a long look.
        adaptedLog2_ = eye_adaptation::adapt(adaptedLog2_, measured, std::clamp(dt, 0.0f, 0.1f), params_);
        exposure_ = eye_adaptation::exposureFor(adaptedLog2_, params_);
        if (log_) {
            logTimer_ += dt;
            if (logTimer_ >= 1.0f) {
                logTimer_ = 0.0f;
                LOG_WARNING("EyeAdaptation: measured ", measured, " adapted ", adaptedLog2_, " exposure ",
                            exposure_);
            }
        }
    }

    const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                             VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
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
        b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                              .baseArrayLayer = 0, .layerCount = 1};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    };

    // The finished frame, as the sun shafts find it: in PRESENT_SRC, and
    // perhaps just read by them or by the water's copy.
    barrier(swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    // Its last contents went to the buffer two frames ago.
    barrier(small_[frame].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0,
                           .layerCount = 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[0] = {.x = static_cast<int32_t>(float(extent.width) * kRegionLeft),
                          .y = static_cast<int32_t>(float(extent.height) * kRegionTop), .z = 0};
    blit.srcOffsets[1] = {.x = static_cast<int32_t>(float(extent.width) * kRegionRight),
                          .y = static_cast<int32_t>(float(extent.height) * kRegionBottom), .z = 1};
    blit.dstOffsets[1] = {.x = static_cast<int32_t>(kWidth), .y = static_cast<int32_t>(kHeight), .z = 1};
    vkCmdBlitImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small_[frame].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // Back for the overlay pass, which loads it from PRESENT_SRC.
    barrier(swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(small_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy copy{};
    copy.imageSubresource = blit.dstSubresource;
    copy.imageExtent = {.width = kWidth, .height = kHeight, .depth = 1};
    vkCmdCopyImageToBuffer(cmd, small_[frame].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_[frame].buffer, 1, &copy);

    VkBufferMemoryBarrier2 toHost{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = readback_[frame].buffer;
    toHost.offset = 0;
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &toHost;
    cmdPipelineBarrier2(cmd, dep);

    measured_[frame] = true;
}

void EyeAdaptationPass::composite(VkCommandBuffer cmd) {
    if (!usable_ || compositePipeline_ == VK_NULL_HANDLE || std::abs(exposure_ - 1.0f) < kNeutral) return;
    const float halfExposure = 0.5f * exposure_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
    vkCmdPushConstants(cmd, compositeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &halfExposure);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void EyeAdaptationPass::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    vkDeviceWaitIdle(device);
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroyScreenTarget(*ctx_, small_[i]);
        if (readback_[i].buffer) vmaDestroyBuffer(ctx_->getAllocator(), readback_[i].buffer, readback_[i].allocation);
        readback_[i] = {};
        measured_[i] = false;
    }
    destroy(device, compositePipeline_);
    destroy(device, compositeLayout_);
    compositeFormat_ = VK_FORMAT_UNDEFINED;
    usable_ = false;
    ctx_ = nullptr;
}

} // namespace rendering
} // namespace wowee
