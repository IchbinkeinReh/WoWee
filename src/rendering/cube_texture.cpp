#include "rendering/cube_texture.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace wowee {
namespace rendering {

namespace {
constexpr uint32_t kFaceCount = 6;
}  // namespace

bool uploadCubeTexture(VkContext& ctx, const uint8_t* rgba, uint32_t width, uint32_t height,
                       CubeTexture& out) {
    if (!rgba || width == 0 || height == 0) return false;
    const bool strip = isCubeStrip(width, height);
    const uint32_t size = strip ? height : std::max(width, height);
    constexpr size_t kBytesPerPixel = 4;  // RGBA8
    std::vector<uint8_t> faces(static_cast<size_t>(size) * size * kBytesPerPixel * kFaceCount);
    for (uint32_t f = 0; f < kFaceCount; ++f) {
        const uint32_t square = static_cast<uint32_t>(cubeStripSquare(static_cast<int>(f)));
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const uint32_t sx = strip ? square * size + x : x * width / size;
                const uint32_t sy = strip ? y : y * height / size;
                const size_t facePixel = (static_cast<size_t>(f) * size + y) * size + x;
                const size_t sourcePixel = static_cast<size_t>(sy) * width + sx;
                std::memcpy(&faces[facePixel * kBytesPerPixel], &rgba[sourcePixel * kBytesPerPixel],
                            kBytesPerPixel);
            }
        }
    }

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {size, size, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kFaceCount;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    CubeTexture cube;
    if (vmaCreateImage(ctx.getAllocator(), &ii, &aci, &cube.image, &cube.allocation, nullptr) != VK_SUCCESS)
        return false;

    AllocatedBuffer staging = createBuffer(ctx.getAllocator(), faces.size(),
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* mapped = nullptr;
    vmaMapMemory(ctx.getAllocator(), staging.allocation, &mapped);
    std::memcpy(mapped, faces.data(), faces.size());
    vmaUnmapMemory(ctx.getAllocator(), staging.allocation);
    const VkImage image = cube.image;
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kFaceCount};
        region.imageExtent = {size, size, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    });
    if (ctx.isInUploadBatch()) {
        ctx.deferStagingCleanup(staging);
    } else {
        destroyBuffer(ctx.getAllocator(), staging);
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = cube.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};
    if (vkCreateImageView(ctx.getDevice(), &vi, nullptr, &cube.view) != VK_SUCCESS) {
        vmaDestroyImage(ctx.getAllocator(), cube.image, cube.allocation);
        return false;
    }
    out = cube;
    return true;
}

void destroyCubeTexture(VkContext& ctx, CubeTexture& cube) {
    if (cube.view) vkDestroyImageView(ctx.getDevice(), cube.view, nullptr);
    if (cube.image) vmaDestroyImage(ctx.getAllocator(), cube.image, cube.allocation);
    cube = CubeTexture{};
}

}  // namespace rendering
}  // namespace wowee
