#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

#include "rendering/vk_context.hpp"

namespace wowee {
namespace rendering {

/// A sampled view of the scene's depth image for a pass after the scene - the
/// sun shafts' sky mask and the eye adaptation's clip mask both read it to
/// tell the sky from what is drawn. Made anew when the image or its
/// generation changes (the scene's targets made again); the old view goes
/// once no frame in flight can use it. `serial` counts the views made, so a
/// descriptor set can tell it was written with an older one (a destroyed
/// view's handle can be given out again).
struct SceneDepthView {
    VkImage image = VK_NULL_HANDLE;
    uint64_t generation = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t serial = 0;

    /// The view of `depthImage`, or none when it cannot be made.
    VkImageView viewFor(VkContext& ctx, VkImage depthImage, uint64_t depthGeneration) {
        if (depthImage == image && depthGeneration == generation && view != VK_NULL_HANDLE) return view;
        VkDevice device = ctx.getDevice();
        if (view != VK_NULL_HANDLE) {
            VkImageView old = view;
            ctx.deferAfterAllFrameFences([device, old]() { vkDestroyImageView(device, old, nullptr); });
            view = VK_NULL_HANDLE;
        }
        image = depthImage;
        generation = depthGeneration;
        VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depthImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ctx.getDepthFormat();
        vi.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1,
                               .baseArrayLayer = 0, .layerCount = 1};
        if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) {
            view = VK_NULL_HANDLE;
            image = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        ++serial;
        return view;
    }

    /// At shutdown, the device idle.
    void destroy(VkDevice device) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
    }
};

}  // namespace rendering
}  // namespace wowee
