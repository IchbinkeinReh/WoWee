#pragma once

/// A colour image the size of (part of) the screen, with its view - what the
/// passes that copy the finished frame (SunShafts, ScreenEffects) build their
/// copies and results in.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "rendering/vk_context.hpp"

namespace wowee {
namespace rendering {

struct ScreenTarget {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

inline void destroyScreenTarget(VkContext& ctx, ScreenTarget& t) {
    if (t.view) { vkDestroyImageView(ctx.getDevice(), t.view, nullptr); t.view = VK_NULL_HANDLE; }
    if (t.image) { vmaDestroyImage(ctx.getAllocator(), t.image, t.alloc); t.image = VK_NULL_HANDLE; t.alloc = VK_NULL_HANDLE; }
}

/// One mip, one layer, optimal tiling, device local. False and nothing held
/// when either the image or its view cannot be made.
inline bool createScreenTarget(VkContext& ctx, ScreenTarget& t, VkExtent2D size, VkFormat format,
                               VkImageUsageFlags usage) {
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = format;
    imgCI.extent = {.width = size.width, .height = size.height, .depth = 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = usage;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(ctx.getAllocator(), &imgCI, &allocCI, &t.image, &t.alloc, nullptr) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = t.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = format;
    viewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    if (vkCreateImageView(ctx.getDevice(), &viewCI, nullptr, &t.view) != VK_SUCCESS) {
        destroyScreenTarget(ctx, t);
        return false;
    }
    return true;
}

}  // namespace rendering
}  // namespace wowee
