#include "rendering/loading_screen.hpp"
#include "rendering/loading_screen_layout.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <SDL3/SDL_vulkan.h>
#include "rendering/vk_context.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>
#include <cstring>

// The one definition of stb_image the other image readers link against.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace wowee {
namespace rendering {

LoadingScreen::LoadingScreen() = default;

LoadingScreen::~LoadingScreen() {
    shutdown();
}

bool LoadingScreen::initialize(pipeline::AssetManager* assets, uint32_t mapId) {
    LOG_INFO("Initializing loading screen (Vulkan/ImGui)");
    namespace ls = loading_screen;
    // 0x00409ed0: the map's LoadingScreens row (Map +0x24), its "Wide"
    // version on a screen wider than 4:3 where it has one, else the row's
    // picture, else the default.
    std::string fileName;
    bool hasWide = false;
    if (assets && assets->isInitialized()) {
        const auto* layouts = pipeline::getActiveDBCLayout();
        const auto* mapLayout = layouts ? layouts->getLayout("Map") : nullptr;
        const auto* screenLayout = layouts ? layouts->getLayout("LoadingScreens") : nullptr;
        const uint32_t screenField = mapLayout ? mapLayout->tryField("LoadingScreenID") : 0xFFFFFFFFu;
        auto mapDbc = screenField != 0xFFFFFFFFu ? assets->loadDBC("Map.dbc") : nullptr;
        auto screenDbc = screenLayout ? assets->loadDBC("LoadingScreens.dbc") : nullptr;
        if (mapDbc && mapDbc->isLoaded() && screenDbc && screenDbc->isLoaded() &&
            screenField < mapDbc->getFieldCount()) {
            const int32_t mapRow = mapDbc->findRecordById(mapId);
            const uint32_t screenId = mapRow >= 0 ? mapDbc->getUInt32(static_cast<uint32_t>(mapRow), screenField) : 0;
            const int32_t screenRow = screenId ? screenDbc->findRecordById(screenId) : -1;
            const uint32_t fileField = screenLayout->tryField("FileName");
            const uint32_t wideField = screenLayout->tryField("HasWideScreen");
            if (screenRow >= 0 && fileField < screenDbc->getFieldCount()) {
                fileName = screenDbc->getString(static_cast<uint32_t>(screenRow), fileField);
                hasWide = wideField < screenDbc->getFieldCount() &&
                          screenDbc->getUInt32(static_cast<uint32_t>(screenRow), wideField) != 0;
            }
        }
    }
    float screenAspect = 0.0f;
    if (vkCtx) {
        const VkExtent2D extent = vkCtx->getSwapchainExtent();
        if (extent.height > 0) screenAspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    }
    widePicture_ = false;
    if (!fileName.empty() && ls::wantsWidePicture(screenAspect, hasWide))
        widePicture_ = loadTexture(assets, ls::widePicturePath(fileName), picture_);
    if (!widePicture_ && !fileName.empty()) loadTexture(assets, fileName, picture_);
    if (!picture_.descriptor) loadTexture(assets, ls::kDefaultPicture, picture_);
    for (size_t i = 0; i < ls::kBar.size(); ++i) loadTexture(assets, ls::kBar[i].texture, bar_[i]);
    LOG_INFO("Loading screen initialized: map ", mapId, " picture '", fileName, "'", widePicture_ ? " (wide)" : "");
    return true;
}

void LoadingScreen::setStatus(const std::string& status) {
    LOG_DEBUG("Loading: ", status);
}

void LoadingScreen::release(Texture& tex) {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    // ImGui manages the descriptor set's lifetime.
    tex.descriptor = VK_NULL_HANDLE;
    if (tex.view) { vkDestroyImageView(device, tex.view, nullptr); tex.view = VK_NULL_HANDLE; }
    if (tex.image) { vkDestroyImage(device, tex.image, nullptr); tex.image = VK_NULL_HANDLE; }
    if (tex.memory) { vkFreeMemory(device, tex.memory, nullptr); tex.memory = VK_NULL_HANDLE; }
}

void LoadingScreen::shutdown() {
    if (!vkCtx) return;
    if (!picture_.image && !bar_[0].image && !bar_[1].image) return;
    vkDeviceWaitIdle(vkCtx->getDevice());
    release(picture_);
    for (Texture& tex : bar_) release(tex);
}

static uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOG_ERROR("LoadingScreen: no suitable memory type found");
    return UINT32_MAX;
}

bool LoadingScreen::loadTexture(pipeline::AssetManager* assets, const std::string& path, Texture& out) {
    if (!assets || !assets->isInitialized()) return false;
    std::string blp = path;
    if (blp.size() < 4 || (blp.compare(blp.size() - 4, 4, ".blp") != 0 && blp.compare(blp.size() - 4, 4, ".BLP") != 0))
        blp += ".blp";
    pipeline::BLPImage image = assets->loadTexture(blp);
    if (!image.isValid() || image.data.empty()) {
        LOG_WARNING("Loading screen: no texture at ", blp);
        return false;
    }
    if (out.image) {
        vkDeviceWaitIdle(vkCtx->getDevice());
        release(out);
    }
    return upload(image.data.data(), image.width, image.height, out);
}

bool LoadingScreen::upload(const uint8_t* rgba, int imageWidth, int imageHeight, Texture& out) {
    if (!vkCtx) {
        LOG_WARNING("No VkContext for loading screen image");
        return false;
    }
    VkDevice device = vkCtx->getDevice();
    VkPhysicalDevice physDevice = vkCtx->getPhysicalDevice();
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(imageWidth) * imageHeight * 4;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = imageSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mapped;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
        std::memcpy(mapped, rgba, imageSize);
        vkUnmapMemory(device, stagingMemory);
    }

    // Create image
    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {.width = static_cast<uint32_t>(imageWidth), .height = static_cast<uint32_t>(imageHeight), .depth = 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &imgInfo, nullptr, &out.image);

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, out.image, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &out.memory);
        vkBindImageMemory(device, out.image, out.memory, 0);
    }

    // Transfer: transition, copy, transition
    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        // Transition to transfer dst
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = out.image;
        barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkDependencyInfo barrierDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        barrierDep.dependencyFlags = 0;
        barrierDep.imageMemoryBarrierCount = 1;
        barrierDep.pImageMemoryBarriers = &barrier;
        cmdPipelineBarrier2(cmd, barrierDep);

        // Copy buffer to image
        VkBufferImageCopy region{};
        region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        region.imageExtent = {.width = static_cast<uint32_t>(imageWidth), .height = static_cast<uint32_t>(imageHeight), .depth = 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, out.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition to shader read
        barrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo toReadDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        toReadDep.imageMemoryBarrierCount = 1;
        toReadDep.pImageMemoryBarriers = &barrier;
        cmdPipelineBarrier2(cmd, toReadDep);
    });

    // Cleanup staging
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Create image view
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = out.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCreateImageView(device, &viewInfo, nullptr, &out.view);
    }

    // Create sampler
    VkSampler sampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler = vkCtx->getOrCreateSampler(samplerInfo);  // owned by VkContext's cache
    }

    // Register with ImGui as a texture
    out.descriptor = ImGui_ImplVulkan_AddTexture(sampler, out.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return true;
}

void LoadingScreen::render() {
    // If a frame is already in progress (e.g. called from a UI callback),
    // end it before starting our own
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->FrameCount >= 0 && ctx->WithinFrameScope) {
        ImGui::EndFrame();
    }

    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // A fullscreen window to draw in. NoInputs keeps it from swallowing
    // clicks, NoBringToFrontOnFocus from climbing over other windows.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##LoadingScreen", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 0x0040a270: the screen black, the picture in a viewport of its own
    // shape, and on it the bar - its fill, then its border (0x004090c0).
    namespace ls = loading_screen;
    const float aspect = screenH > 0.0f ? screenW / screenH : 0.0f;
    const ls::Rect view = ls::pictureViewport(aspect, widePicture_);
    // A rectangle of the viewport's 0..1, y up, in ImGui's pixels, y down.
    auto toScreen = [&](const ls::Rect& r, ImVec2& p0, ImVec2& p1) {
        const float vx = view.x0 * screenW, vw = (view.x1 - view.x0) * screenW;
        const float vy = view.y0 * screenH, vh = (view.y1 - view.y0) * screenH;
        p0 = ImVec2(vx + r.x0 * vw, screenH - (vy + r.y1 * vh));
        p1 = ImVec2(vx + r.x1 * vw, screenH - (vy + r.y0 * vh));
    };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (picture_.descriptor) {
        ImVec2 p0, p1;
        toScreen(ls::Rect{}, p0, p1);
        drawList->AddImage(reinterpret_cast<ImTextureID>(picture_.descriptor), p0, p1);
    }
    for (size_t i = 0; i < ls::kBar.size(); ++i) {
        if (!bar_[i].descriptor) continue;
        ImVec2 p0, p1;
        toScreen(ls::barPieceRect(ls::kBar[i], loadProgress), p0, p1);
        if (p1.x <= p0.x) continue;
        drawList->AddImage(reinterpret_cast<ImTextureID>(bar_[i].descriptor), p0, p1);
    }

    ImGui::End();
    ImGui::Render();

    // Submit the frame to Vulkan (loading screen runs outside the main render loop)
    if (vkCtx) {
        // Handle window resize: recreate swapchain before acquiring an image
        if (vkCtx->isSwapchainDirty() && sdlWindow) {
            // The surface, in pixels. SDL_GetWindowSize answers points, and
            // on a high density display rebuilding at those halves the
            // swapchain under a loading screen that is drawn full width.
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(sdlWindow, &w, &h);
            if (w > 0 && h > 0) {
                (void)vkCtx->recreateSwapchain(w, h);
            }
        }

        uint32_t imageIndex = 0;
        VkCommandBuffer cmd = vkCtx->beginFrame(imageIndex);
        if (cmd != VK_NULL_HANDLE) {
            // Begin render pass
            // The UI draws in the overlay pass, which is what ImGui's pipelines
            // are built for: single-sampled and colour only. Drawing it in the
            // scene pass instead put a 1x pipeline inside an 8x pass, which the
            // validation layers reject and the driver renders as it pleases.
            // This variant clears, since there is no scene underneath here.
            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass = vkCtx->getOverlayClearRenderPass();
            rpInfo.framebuffer = vkCtx->getOverlayFramebuffers()[imageIndex];
            rpInfo.renderArea.offset = {.x = 0, .y = 0};
            rpInfo.renderArea.extent = vkCtx->getSwapchainExtent();

            VkClearValue clearValues[1]{};
            clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            rpInfo.clearValueCount = 1;
            rpInfo.pClearValues = clearValues;

            const bool overlayReady =
                rpInfo.renderPass != VK_NULL_HANDLE &&
                imageIndex < vkCtx->getOverlayFramebuffers().size();
            if (overlayReady) {
                vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
                vkCmdEndRenderPass(cmd);
            }

            vkCtx->endFrame(cmd, imageIndex);
        }
    }
}

} // namespace rendering
} // namespace wowee
