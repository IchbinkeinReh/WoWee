#include "rendering/renderer.hpp"
#include "rendering/sun_direction.hpp"
#include "rendering/wmo_sidn.hpp"

#include <fstream>
#include <iterator>
#include "addons/lua_api_registrations.hpp"
#include "core/env_flag.hpp"
#include "rendering/sky_params_from_lighting.hpp"
#include "core/coordinates.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/custom_zone_discovery.hpp"
#include "rendering/performance_hud.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/day_night.hpp"
#include "core/memory_monitor.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/weather.hpp"
#include "rendering/lightning.hpp"
#include "rendering/lighting_manager.hpp"
#include "core/profiler.hpp"
#include "core/frame_profiler.hpp"
#include "core/thread_pool.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/mount_dust.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/grass_profile.hpp"
#include "pipeline/grass_population.hpp"
#include "pipeline/grass_terrain.hpp"
#include "rendering/grass_renderer.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/volumetric_fog.hpp"
#include "rendering/sun_shafts.hpp"
#include "rendering/screen_effects.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/screen_capture.hpp"
#include "rendering/loot_sparkles.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/footprint_renderer.hpp"
#include "rendering/fishing_line.hpp"
#include "rendering/blob_shadow_renderer.hpp"
#include "rendering/selection_circle.hpp"
#include "rendering/spell_target_circle.hpp"
#include "game/game_handler.hpp"
#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "core/logger.hpp"
#include "game/world.hpp"
#include "game/zone_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/amd_fsr3_runtime.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/render_graph.hpp"
#include "rendering/overlay_system.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cctype>
#include <cmath>
#include <chrono>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <future>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace wowee {
namespace rendering {

namespace {
/// The light-space depth every cascade shares, near and far, in yards from
/// the light: from a yard in front of it to 6.5 x the shadow distance.
constexpr float kShadowNearPlane = 1.0f;
float shadowFarPlane(float distance) { return distance * 6.5f; }
float shadowDepthRange(float distance) { return shadowFarPlane(distance) - kShadowNearPlane; }
}  // namespace


Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::createShadowMapImages() {
    VkDevice device = vkCtx->getDevice();

    // --- Create per-frame shadow depth images (one per in-flight frame) ---
    // Each frame slot has its own depth image so that frame N's shadow read and
    // frame N+1's shadow write cannot race on the same image. Each is the
    // whole atlas, every cascade's tile in it.
    const uint32_t atlasW = shadowAtlasWidth(shadowCascadeCount_, SHADOW_MAP_SIZE);
    const uint32_t atlasH = shadowAtlasHeight(shadowCascadeCount_, SHADOW_MAP_SIZE);
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_D32_SFLOAT;
    imgCI.extent = {.width = atlasW, .height = atlasH, .depth = 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo imgAllocCI{};
    imgAllocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (vmaCreateImage(vkCtx->getAllocator(), &imgCI, &imgAllocCI,
                &shadowDepthImage[i], &shadowDepthAlloc[i], nullptr) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image [", i, "] at ",
                      atlasW, "x", atlasH);
            return false;
        }
        setObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(shadowDepthImage[i]), "shadow atlas");
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // --- Create per-frame shadow depth image views ---
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_D32_SFLOAT;
    viewCI.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        viewCI.image = shadowDepthImage[i];
        if (vkCreateImageView(device, &viewCI, nullptr, &shadowDepthView[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image view [", i, "]");
            return false;
        }
    }

    // --- Create per-frame shadow framebuffers ---
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = shadowRenderPass;
    fbCI.attachmentCount = 1;
    fbCI.width = atlasW;
    fbCI.height = atlasH;
    fbCI.layers = 1;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        fbCI.pAttachments = &shadowDepthView[i];
        if (vkCreateFramebuffer(device, &fbCI, nullptr, &shadowFramebuffer[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow framebuffer [", i, "]");
            return false;
        }
    }
    return true;
}

void Renderer::destroyShadowMapImages() {
    VkDevice device = vkCtx->getDevice();
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (shadowFramebuffer[i]) { vkDestroyFramebuffer(device, shadowFramebuffer[i], nullptr); shadowFramebuffer[i] = VK_NULL_HANDLE; }
        if (shadowDepthView[i]) { vkDestroyImageView(device, shadowDepthView[i], nullptr); shadowDepthView[i] = VK_NULL_HANDLE; }
        if (shadowDepthImage[i]) { vmaDestroyImage(vkCtx->getAllocator(), shadowDepthImage[i], shadowDepthAlloc[i]); shadowDepthImage[i] = VK_NULL_HANDLE; shadowDepthAlloc[i] = VK_NULL_HANDLE; }
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

bool Renderer::createPerFrameResources() {
    VkDevice device = vkCtx->getDevice();

    // --- Create shadow sampler (shared - read-only, no per-frame needed) ---
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampCI.compareEnable = VK_TRUE;
    sampCI.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowSampler = vkCtx->getOrCreateSampler(sampCI);
    if (shadowSampler == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create shadow sampler");
        return false;
    }

    // --- Create shadow render pass (depth-only) ---
    VkAttachmentDescription depthAtt{};
    depthAtt.format = VK_FORMAT_D32_SFLOAT;
    depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments = &depthAtt;
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;
    if (vkCreateRenderPass(device, &rpCI, nullptr, &shadowRenderPass) != VK_SUCCESS) {
        LOG_ERROR("Failed to create shadow render pass");
        return false;
    }

    // --- Create the per-frame shadow maps themselves ---
    // After the render pass, which their framebuffers are made against. The
    // same function rebuilds them when the resolution setting changes.
    // The quality level's side is at most 4096, which every device makes; two
    // cascades side by side are twice that across, which not every one does.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(vkCtx->getPhysicalDevice(), &props);
        shadowAtlasMaxWidth_ = std::min({props.limits.maxImageDimension2D,
                                         props.limits.maxFramebufferWidth,
                                         props.limits.maxViewportDimensions[0]});
    }
    SHADOW_MAP_SIZE = std::min(SHADOW_MAP_SIZE, maxShadowMapSize(shadowCascadeCount_));
    if (!createShadowMapImages()) return false;

    // The fog's sampler and neutral volume come first: the layout below bakes
    // the one in, and every set written below binds the other until the fog
    // has volumes of its own.
    volumetricFog_ = std::make_unique<VolumetricFog>();
    if (!volumetricFog_->initialize(vkCtx)) {
        LOG_ERROR("Failed to create the volumetric fog's sampler and neutral volume");
        return false;
    }

    // The ray traced lighting's scene and pass. They outlive the per-frame
    // sets: the renderers register geometry with the scene as they load, and
    // bindings 3 and 4 below take the pass's sampler as immutable.
    if (!rtScene_) {
        rtScene_ = std::make_unique<RtScene>();
        if (!rtScene_->initialize(vkCtx)) {
            LOG_ERROR("Failed to create the ray tracing scene");
            return false;
        }
        rtLighting_ = std::make_unique<RtLighting>();
        if (!rtLighting_->initialize(vkCtx, rtScene_.get())) {
            LOG_ERROR("Failed to create the ray traced lighting");
            return false;
        }
    }

    // --- Create descriptor set layout for set 0 (per-frame UBO + shadow sampler + fog volume
    //     + ray traced lighting) ---
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    // Compute as well: the fog volume is lit from this same block.
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Immutable, because this one compares. Portability implementations may
    // report VkPhysicalDevicePortabilitySubsetFeaturesKHR::mutableComparisonSamplers
    // as false -- MoltenVK does -- and then a sampler with compareEnable set is
    // only legal here, baked into the layout, rather than written into the
    // descriptor per frame. shadowSampler is created above this point.
    bindings[1].pImmutableSamplers = &shadowSampler;
    // The fog volume, read per fragment by surfaces and per vertex by
    // particles and ribbons. Immutable like binding 1, so a set allocated
    // anywhere else - the character preview's - only has to name a view.
    const VkSampler fogSampler = volumetricFog_->getSampler();
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].pImmutableSamplers = &fogSampler;
    // Last frame's ray traced lighting (RtLighting), read by the terrain,
    // building, doodad and character surfaces. Immutable samplers again, so
    // the preview's sets only name the neutral view.
    const VkSampler rtSampler = rtLighting_->sampler();
    for (uint32_t b = 3; b <= 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[b].pImmutableSamplers = &rtSampler;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &perFrameSetLayout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create per-frame descriptor set layout");
        return false;
    }

    // --- Create descriptor pool for UBO + image sampler (normal frames + reflection) ---
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES * 2; // normal frames + reflection frames
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES * 2 * 4;  // shadow, fog, two RT results, per set

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES * 2; // normal frames + reflection frames
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &sceneDescriptorPool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create scene descriptor pool");
        return false;
    }

    // --- Create per-frame UBOs and descriptor sets ---
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        // Create mapped UBO
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                &perFrameUBOs[i], &perFrameUBOAllocs[i], &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create per-frame UBO ", i);
            return false;
        }
        perFrameUBOMapped[i] = mapInfo.pMappedData;

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameSetLayout;

        if (vkAllocateDescriptorSets(device, &setAlloc, &perFrameDescSets[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate per-frame descriptor set ", i);
            return false;
        }

        // Write binding 0 (UBO) and binding 1 (shadow sampler)
        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = perFrameUBOs[i];
        descBuf.offset = 0;
        descBuf.range = sizeof(GPUPerFrameData);

        VkDescriptorImageInfo shadowImgInfo{};
        // sampler is ignored: binding 1 declares it immutable in the layout.
        shadowImgInfo.imageView = shadowDepthView[i];
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Neutral until the fog is switched on; writeFogVolumeBindings swaps
        // in this slot's own volume then.
        VkDescriptorImageInfo fogImgInfo{};
        fogImgInfo.imageView = volumetricFog_->getVolumeView(i);
        fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Neutral until the ray traced lighting is on; writeRtLightingBindings
        // swaps in the other slot's results then.
        VkDescriptorImageInfo rtImgInfo[2]{};
        rtImgInfo[0].imageView = rtLighting_->lightViewForSlot(i);
        rtImgInfo[1].imageView = rtLighting_->giViewForSlot(i);
        rtImgInfo[0].imageLayout = rtImgInfo[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = perFrameDescSets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = perFrameDescSets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImgInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = perFrameDescSets[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &fogImgInfo;
        for (uint32_t b = 0; b < 2; ++b) {
            writes[3 + b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3 + b].dstSet = perFrameDescSets[i];
            writes[3 + b].dstBinding = 3 + b;
            writes[3 + b].descriptorCount = 1;
            writes[3 + b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3 + b].pImageInfo = &rtImgInfo[b];
        }

        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    }

    // --- Create reflection per-frame UBO and descriptor set ---
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                &reflPerFrameUBO, &reflPerFrameUBOAlloc, &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create reflection per-frame UBO");
            return false;
        }
        reflPerFrameUBOMapped = mapInfo.pMappedData;

        VkDescriptorSetLayout layouts[MAX_FRAMES];
        for (auto& layout : layouts) layout = perFrameSetLayout;

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = MAX_FRAMES;
        setAlloc.pSetLayouts = layouts;

        if (vkAllocateDescriptorSets(device, &setAlloc, reflPerFrameDescSet) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate reflection per-frame descriptor sets");
            return false;
        }

        // Bind each reflection descriptor to the same UBO but its own frame's shadow view
        for (uint32_t i = 0; i < MAX_FRAMES; i++) {
            VkDescriptorBufferInfo descBuf{};
            descBuf.buffer = reflPerFrameUBO;
            descBuf.offset = 0;
            descBuf.range = sizeof(GPUPerFrameData);

            VkDescriptorImageInfo shadowImgInfo{};
            // sampler is ignored: binding 1 declares it immutable in the layout.
            shadowImgInfo.imageView = shadowDepthView[i];
            shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // Always neutral: the volume is built for the camera, and the
            // mirrored one would read it at the wrong place. The reflection's
            // block switches the fog off.
            VkDescriptorImageInfo fogImgInfo{};
            fogImgInfo.imageView = volumetricFog_->getNeutralView();
            fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            // Neutral as well: the result is the camera's, and the block's
            // switch is off in the reflection's copy.
            VkDescriptorImageInfo rtImgInfo{};
            rtImgInfo.imageView = rtLighting_->neutralView();
            rtImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[5]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = reflPerFrameDescSet[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &descBuf;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = reflPerFrameDescSet[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &shadowImgInfo;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = reflPerFrameDescSet[i];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &fogImgInfo;
            for (uint32_t b = 3; b <= 4; ++b) {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = reflPerFrameDescSet[i];
                writes[b].dstBinding = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b].pImageInfo = &rtImgInfo;
            }

            vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
        }
    }

    // The compute side. Not fatal: without it the fog simply stays off, and
    // applyPendingQuality says so when it is asked for.
    if (!volumetricFog_->createPipelines(perFrameSetLayout, shadowDepthView)) {
        LOG_WARNING("Volumetric fog pipelines failed to build - volumetric fog unavailable");
    }

    LOG_INFO("Per-frame Vulkan resources created (shadow atlas ",
             shadowAtlasWidth(shadowCascadeCount_, SHADOW_MAP_SIZE), "x",
             shadowAtlasHeight(shadowCascadeCount_, SHADOW_MAP_SIZE), ", ",
             shadowCascadeCount_, " cascade(s) of ", SHADOW_MAP_SIZE, ")");
    return true;
}

void Renderer::destroyPerFrameResources() {
    if (!vkCtx) return;
    vkDeviceWaitIdle(vkCtx->getDevice());
    VkDevice device = vkCtx->getDevice();

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        destroy(vkCtx->getAllocator(), perFrameUBOs[i], perFrameUBOAllocs[i]);
    }
    if (reflPerFrameUBO) {
        vmaDestroyBuffer(vkCtx->getAllocator(), reflPerFrameUBO, reflPerFrameUBOAlloc);
        reflPerFrameUBO = VK_NULL_HANDLE;
        reflPerFrameUBOMapped = nullptr;
    }
    // Before the layout: its compute pipeline layout was built from it.
    if (volumetricFog_) {
        volumetricFog_->shutdown();
        volumetricFog_.reset();
    }
    destroy(device, sceneDescriptorPool);
    destroy(device, perFrameSetLayout);
    // After every renderer has let go of its geometry, which is why this is
    // here rather than beside the fog: shutdown() calls this last.
    if (rtLighting_) {
        rtLighting_->shutdown();
        rtLighting_.reset();
    }
    if (rtScene_) {
        rtScene_->shutdown();
        rtScene_.reset();
    }

    // Destroy per-frame shadow resources
    destroyShadowMapImages();
    if (shadowRenderPass) { vkDestroyRenderPass(device, shadowRenderPass, nullptr); shadowRenderPass = VK_NULL_HANDLE; }
    shadowSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
}

void Renderer::updatePerFrameUBO() {
    if (!camera) return;

    currentFrameData.view = camera->getViewMatrix();
    currentFrameData.projection = camera->getProjectionMatrix();
    // w: the close-up texture filter, 1 + the mode (texture_filter.glsli).
    currentFrameData.viewPos = glm::vec4(camera->getPosition(), 1.0f + static_cast<float>(textureMagnification_));
    currentFrameData.fogParams.z = globalTime;

    // Lighting from LightingManager
    if (lightingManager) {
        const auto& lp = lightingManager->getLightingParams();
        currentFrameData.lightDir = glm::vec4(lp.directionalDir, 0.0f);
        // Looking into the sun's glare darkens the world: the ambient and
        // the direct light by up to 35% (0x007816f0 with the glare's
        // 0xd38f4c from the frame before, 0x007ef6e0).
        const float glareDim = skySystem ? 1.0f - skySystem->getSunGlareDim() : 1.0f;
        currentFrameData.lightColor = glm::vec4(lp.diffuseColor * glareDim, 1.0f);
        // w: ch8's red, how dark the terrain's baked shadows are drawn.
        currentFrameData.ambientColor = glm::vec4(lp.ambientColor * glareDim, lp.shadowOpacity);
        // The zone's fog colour for the world, the camera's own - its
        // interior's blended in - for interior groups and what stands in
        // them (0x007f16f0, 0x007a8440). w: the power the fog is drawn
        // with, 1 before map 530 (0x00873210).
        currentFrameData.fogColor = glm::vec4(lp.zoneFogColor, lp.fogExponent);
        currentFrameData.cameraFogColor = glm::vec4(lp.fogColor, lp.fogExponent);
        // Not dimmed by the glare: 0x007816f0 darkens only 0xd38ca8/cac.
        currentFrameData.averagedDirectColor = glm::vec4(lp.averagedDirectColor, 1.0f);
        currentFrameData.averagedAmbientColor = glm::vec4(lp.averagedAmbientColor, 1.0f);
        currentFrameData.windowLight = glm::vec4(
            wmo_sidn::windowLevel(lightingManager->getTimeOfDay()), 0.0f, 0.0f, 0.0f);
        currentFrameData.specularColor = glm::vec4(lp.sunColor, clientSpecular_ ? 1.0f : 0.0f);
        currentFrameData.fogParams.x = lp.fogStart;
        currentFrameData.fogParams.y = lp.fogEnd;

        // Under water the fog is the light's own: the underwater set, darkened
        // with depth by the liquid's LiquidType row (LightingManager::update,
        // 0x007f3230). There is no fixed blue fog in the client.
    }

    // The cascades, as shadow_csm.glsli reads them. None before the player
    // has a position (computeLightSpaceMatrix gave a zero matrix and the
    // shadow pass skips itself), which the shaders read as lit.
    {
        const int cascades = (lightSpaceMatrix == glm::mat4(0.0f)) ? 0 : activeShadowCascades_;
        const uint32_t atlasW = shadowAtlasWidth(shadowCascadeCount_, SHADOW_MAP_SIZE);
        const uint32_t atlasH = shadowAtlasHeight(shadowCascadeCount_, SHADOW_MAP_SIZE);
        for (int c = 0; c < GPUPerFrameData::kMaxShadowCascades; ++c) {
            if (c < cascades) {
                const ShadowCascade& sc = shadowCascades_[c];
                currentFrameData.cascadeMatrix[c] = sc.atlas;
                currentFrameData.cascadeRect[c] = glm::vec4(
                    static_cast<float>(sc.tile.offset.x) / static_cast<float>(atlasW),
                    static_cast<float>(sc.tile.offset.y) / static_cast<float>(atlasH),
                    static_cast<float>(sc.tile.offset.x + static_cast<int32_t>(sc.tile.extent.width)) /
                        static_cast<float>(atlasW),
                    static_cast<float>(sc.tile.offset.y + static_cast<int32_t>(sc.tile.extent.height)) /
                        static_cast<float>(atlasH));
                currentFrameData.cascadeTexel[c] = glm::vec4(
                    sc.texelWorld, 1.0f / static_cast<float>(atlasW), 1.0f / static_cast<float>(atlasH), 0.0f);
            } else {
                currentFrameData.cascadeMatrix[c] = glm::mat4(0.0f);
                currentFrameData.cascadeRect[c] = glm::vec4(0.0f);
                currentFrameData.cascadeTexel[c] = glm::vec4(0.0f);
            }
        }
        // y: a cascade blends into the next over its outer 12%; z: the last
        // one fades out from 90% of its half-width to its edge.
        // w: one yard of the shared light-space depth range in depth units,
        // so a shader can bias by a distance (character.frag's self-shadow).
        currentFrameData.cascadeInfo = glm::vec4(static_cast<float>(cascades), 0.12f, 0.9f,
                                                 1.0f / shadowDepthRange(shadowDistance_));
        // The outer cascade's, in the same atlas terms, for anything that
        // reads the one matrix rather than the cascades.
        currentFrameData.lightSpaceMatrix = cascades > 0 ? shadowCascades_[cascades - 1].atlas
                                                         : lightSpaceMatrix;
    }
    // Scale shadow bias proportionally to ortho extent to avoid acne at close range / gaps at far range
    float shadowBias = glm::clamp(0.8f * (shadowDistance_ / 300.0f), 0.0f, 1.0f);
    // z carries one texel of a cascade's tile, as it carried one of the single
    // map; the cascades' own are in cascadeTexel.
    currentFrameData.shadowParams = glm::vec4(shadowsEnabled ? 1.0f : 0.0f, shadowBias,
                                              1.0f / static_cast<float>(SHADOW_MAP_SIZE), 0.0f);

    // Whether this frame builds the fog volume. Decided here, beside the
    // switch in the block the shaders read, and the frame graph dispatches by
    // the same flag - so no shader reads a volume its frame did not build.
    //
    // Only on a frame whose shadow pass draws. The inject pass samples this
    // slot's shadow map, and before the player has a position - the login,
    // realm and character screens - there is none: computeLightSpaceMatrix
    // gives a zero matrix and the shadow pass skips itself. Those frames used
    // to switch the volume on in this block and enable the fog's pass anyway,
    // leaving a layout check made while recording as the only thing between
    // the dispatch and a map that was never drawn. Decided here instead, the
    // screens with no world never record the fog at all.
    volumetricThisFrame_ = volumetricFog_ && volumetricFog_->isOn() &&
                           volumetricFogDensity_ > 0.0f && shadowPassDrawsThisFrame() &&
                           !(passAblation_ && passAblation_->skip(AblationPass::VolumetricFog));
    currentFrameData.volumetricParams = volumetricThisFrame_ ? volumetricFog_->frameParams()
                                                             : glm::vec4(0.0f);

    // No local lights: the client has no dynamic point lights from torches,
    // braziers, glow cards or lava. Its WMO light is baked into the vertex
    // colours.

    if (rtLighting_) {
        const RtLighting::ConsumerData rt = rtLighting_->consumerData();
        currentFrameData.rtViewProj = rt.viewProj;
        currentFrameData.rtCameraPos = rt.cameraPos;
        currentFrameData.rtParams = rt.params;
    }

    // Player motion, consumed by water ripples and by the foliage brush in
    // m2.vert. Horizontal speed only: a fall shouldn't read as running.
    {
        const float dt = std::max(lastDeltaTime_, 0.0f);
        if (!playerMotionTracked_) {
            prevPlayerPos_ = characterPosition;
            playerWakePos_ = characterPosition;
            playerMotionTracked_ = true;
        }
        if (dt > 0.0f) {
            const glm::vec2 step(characterPosition.x - prevPlayerPos_.x,
                                 characterPosition.y - prevPlayerPos_.y);
            // A teleport is not a sprint. Anything past a gallop is discarded
            // rather than smoothed, or one map change flattens a whole field.
            constexpr float MAX_TRACKED_SPEED = 30.0f;
            const float rawSpeed = glm::length(step) / dt;
            playerSpeed_ = (rawSpeed > MAX_TRACKED_SPEED) ? 0.0f
                                                          : glm::mix(playerSpeed_, rawSpeed, 0.25f);

            // Exponential chase, framerate independent.
            constexpr float WAKE_TIME_CONSTANT = 0.30f;  // seconds of springback
            const float k = 1.0f - std::exp(-dt / WAKE_TIME_CONSTANT);
            playerWakePos_ += (characterPosition - playerWakePos_) * k;
            if (rawSpeed > MAX_TRACKED_SPEED) playerWakePos_ = characterPosition;
        }
        prevPlayerPos_ = characterPosition;
    }
    currentFrameData.playerPos = glm::vec4(characterPosition, playerSpeed_);
    currentFrameData.playerWake = glm::vec4(playerWakePos_, 0.0f);

    // Water ripple gate: swimming and actually moving.
    if (cameraController) {
        bool inWater = cameraController->isSwimming();
        bool moving = cameraController->isMoving();
        currentFrameData.fogParams.w = (inWater && moving) ? 1.0f : 0.0f;
    } else {
        currentFrameData.fogParams.w = 0.0f;
    }

    // Copy to current frame's mapped UBO
    uint32_t frame = vkCtx->getCurrentFrame();
    std::memcpy(perFrameUBOMapped[frame], &currentFrameData, sizeof(GPUPerFrameData));
}

bool Renderer::initialize(core::Window* win) {
    window = win;
    // The thread that records frames, and so the one that waits on the doodad
    // worker when something reaches for the renderers it is reading.
    mainThreadId_ = std::this_thread::get_id();
    vkCtx = win->getVkContext();
    deferredWorldInitEnabled_ = core::envFlagEnabled("WOWEE_DEFER_WORLD_SYSTEMS", true);
    LOG_INFO("Initializing renderer (Vulkan)");

    // Create camera (in front of Stormwind gate, looking north)
    camera = std::make_unique<Camera>();
    camera->setPosition(glm::vec3(-8900.0f, -170.0f, 150.0f));
    camera->setRotation(0.0f, -5.0f);
    camera->setAspectRatio(window->getAspectRatio());
    camera->setFov(60.0f);

    // Create camera controller
    cameraController = std::make_unique<CameraController>(camera.get());
    cameraController->setUseWoWSpeed(true);  // Use realistic WoW movement speed
    cameraController->setMouseSensitivity(0.15f);

    // Create performance HUD
    performanceHUD = std::make_unique<PerformanceHUD>();
    performanceHUD->setPosition(PerformanceHUD::Position::TOP_LEFT);

    // What the player last chose for shadow quality, before the resources that
    // bake it in are built. Read from the CVar file rather than waited for:
    // the interface that would normally hand it over does not load until well
    // after this, and by then the shadow map exists at whatever size it was
    // given here. See Renderer::SHADOW_MAP_SIZE.
    {
        // The top two levels are the same size on purpose, and it is worth
        // saying why before someone raises the last one to 8192 as an easy win
        // for modern hardware. Each level doubles the side, so it quadruples
        // the image: at 4096 a depth map is 64 MB and there are two of them,
        // one per frame in flight, which is 128 MB. 8192 would be 512 MB of
        // VRAM for shadows alone - not a step a machine that can run this is
        // certain to have spare, and the allocation failing at start-up is not
        // a path this renderer handles gently. setShadowMapSize clamps to 4096
        // for the same reason. Larger is the shadow resolution setting's to
        // ask for, as a step above this size, and that path falls back to a
        // smaller map when the allocation fails (applyPendingShadowMapSize).
        constexpr uint32_t kShadowSideForLevel[] = {512, 1024, 2048, 4096, 4096};
        // 4096 was the default, and the arithmetic above says what that costs:
        // 64 MB a map, two of them in flight, 128 MB of depth before anything
        // is drawn, refilled every frame. That is a lot to ask of a machine
        // nobody checked, and a good part of the reports of this client running
        // badly are hardware that was never going to carry it. 2048 is a
        // quarter of the fill and 32 MB for the pair, and the slider still
        // reaches the top for anyone who wants to spend it.
        //
        // These are the side of one cascade's tile. The default two cascades
        // sit side by side in an atlas twice as wide, so twice the memory:
        // 64 MB the pair at 2048 (see shadowAtlasWidth).
        //
        // A phone starts lower again: its GPU memory is the system's memory.
#ifdef __ANDROID__
        constexpr const char* kDefaultShadowLevel = "1";   // 1024, 8 MB the pair a cascade
#else
        constexpr const char* kDefaultShadowLevel = "2";   // 2048, 32 MB the pair a cascade
#endif
        const int level = std::clamp(
            std::atoi(addons::storedCVarValue("extShadowQuality", kDefaultShadowLevel).c_str()),
            0, 4);
        extShadowQuality_ = level;
        setShadowMapSize(kShadowSideForLevel[level]);
    }

    // Create per-frame UBO and descriptor sets
    if (!createPerFrameResources()) {
        LOG_ERROR("Failed to create per-frame Vulkan resources");
        return false;
    }

    // Initialize Vulkan sub-renderers (Phase 3)

    // Sky system (owns skybox, starfield, celestial, clouds, lens flare)
    skySystem = std::make_unique<SkySystem>();
    if (!skySystem->initialize(vkCtx, perFrameSetLayout)) {
        LOG_ERROR("Failed to initialize sky system");
        return false;
    }
    // Expose sub-components via renderer accessors
    skybox = nullptr;  // Owned by skySystem; access via skySystem->getSkybox()
    celestial = nullptr;
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    weather = std::make_unique<Weather>();
    if (!weather->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Weather effect initialization failed (non-fatal)");
    // The ground the weather falls to: the highest of the terrain, the liquid
    // on it and anything standing there, within 200 yards above or below
    // (0x007ade10).
    weather->setGroundQuery([this](float x, float y, float z) {
        float h = -std::numeric_limits<float>::infinity();
        auto take = [&](std::optional<float> v) {
            if (v && *v <= z + 200.0f && *v >= z - 200.0f) h = std::max(h, *v);
        };
        if (terrainManager) take(terrainManager->getHeightAt(x, y));
        if (waterRenderer) take(waterRenderer->getNearestWaterHeightAt(x, y, z, 200.0f));
        if (wmoRenderer) take(wmoRenderer->getFloorHeight(x, y, z + 200.0f));
        if (m2Renderer) take(m2Renderer->getFloorHeight(x, y, z + 200.0f));
        return std::isfinite(h) ? h : z - 200.0f;
    });

    lightning = std::make_unique<Lightning>();
    if (!lightning->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Lightning effect initialization failed (non-fatal)");

    swimEffects = std::make_unique<SwimEffects>();
    syncSwimEffectsTargetPass();
    if (!swimEffects->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Swim effect initialization failed (non-fatal)");

    mountDust = std::make_unique<MountDust>();
    if (!mountDust->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Mount dust effect initialization failed (non-fatal)");

    chargeEffect = std::make_unique<ChargeEffect>();
    if (!chargeEffect->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Charge effect initialization failed (non-fatal)");

    fishingLines_ = std::make_unique<FishingLineRenderer>();
    if (!fishingLines_->initialize(this, vkCtx, perFrameSetLayout))
        LOG_WARNING("Fishing line initialization failed (non-fatal)");

    levelUpEffect = std::make_unique<LevelUpEffect>();
    lootSparkles_ = std::make_unique<LootSparkles>();

    // Non-fatal like the effects above: a device that cannot build the compute
    // pipeline still gets everything else, and isReady() gates both call sites.
    grassRenderer_ = std::make_unique<GrassRenderer>();
    if (!grassRenderer_->initialize(vkCtx, perFrameSetLayout))
        LOG_WARNING("Grass renderer initialization failed (non-fatal)");
    // The distance setting may have been applied before this existed.
    grassRenderer_->setCullDistance(grassDistance_);

    questMarkerRenderer = std::make_unique<QuestMarkerRenderer>();
    footprintRenderer = std::make_unique<FootprintRenderer>();
    blobShadowRenderer = std::make_unique<BlobShadowRenderer>();

    LOG_INFO("Vulkan sub-renderers initialized (Phase 3)");

    // LightingManager doesn't use GL - initialize for data-only use
    lightingManager = std::make_unique<LightingManager>();
    auto* assetManager = core::Application::getInstance().getAssetManager();

    // Create zone manager; enrich music paths from DBC if available
    zoneManager = std::make_unique<game::ZoneManager>();
    zoneManager->initialize();
    if (assetManager) {
        zoneManager->enrichFromDBC(assetManager);
    }

    // Audio is now owned by AudioCoordinator (created by Application).
    // Renderer receives AudioCoordinator* via setAudioCoordinator().

    // Create secondary command buffer resources for multithreaded rendering.
    //
    // WOWEE_SINGLE_THREAD_RECORD takes the fallback path instead. That path
    // records the world inline on one thread, which costs CPU time but marks
    // every pass separately - grass included, where the parallel path records
    // grass into the terrain secondary and so reports the two as one number.
    // Running a frame profile both ways says how much of "terrain" is grass,
    // and whether serialising the recording moves the frame time at all.
    if (std::getenv("WOWEE_PASS_ABLATION") != nullptr) {
        passAblation_ = std::make_unique<PassAblation>();
        LOG_WARNING("Pass ablation enabled - ", PassAblation::phaseCount(),
                    " phases over about ",
                    static_cast<int>(passAblation_->expectedRunMs() / 1000.0),
                    "s of being in the world, the first 10s of it settling. Stand "
                    "still outdoors, do not move the camera, and do not quit before "
                    "the table is logged.");
    }

    static const bool forceSingleThread = std::getenv("WOWEE_SINGLE_THREAD_RECORD") != nullptr;
    if (forceSingleThread) {
        LOG_INFO("WOWEE_SINGLE_THREAD_RECORD set - inline recording, one pass per GPU mark");
    } else if (!createSecondaryCommandResources()) {
        LOG_WARNING("Failed to create secondary command buffers - falling back to single-threaded rendering");
    }

    // Create PostProcessPipeline (§4.3 - owns FSR/FXAA/FSR2/FSR3/brightness)
    postProcessPipeline_ = std::make_unique<PostProcessPipeline>();
    postProcessPipeline_->initialize(vkCtx);
    // The ray traced lighting normally records where the water leaves the
    // scene pass. A multisampled scene in an off-screen target never leaves
    // it early, so the pass records here instead, as the upscaler takes over.
    postProcessPipeline_->setSceneClosedHook([this](VkCommandBuffer) {
        if (rtRecordedThisFrame_ || !postProcessPipeline_) return;
        recordRtLighting(postProcessPipeline_->getSceneDepthImage(),
                         postProcessPipeline_->getSceneRenderExtent(),
                         postProcessPipeline_->sceneDepthIsMsaa());
    });

    // Not fatal: without them the picture is only missing its glow.
    screenEffects_ = std::make_unique<ScreenEffects>();
    if (!screenEffects_->initialize(vkCtx)) {
        LOG_WARNING("Screen effects failed to initialise - glow and the death effect unavailable");
        screenEffects_->shutdown();
        screenEffects_.reset();
    }

    // Not fatal: without them the picture is only missing its rays.
    sunShafts_ = std::make_unique<SunShafts>();
    if (!sunShafts_->initialize(vkCtx)) {
        LOG_WARNING("Sun shafts failed to initialise - sun shafts unavailable");
        sunShafts_->shutdown();
        sunShafts_.reset();
    }

    // Create render graph and register virtual resources
    renderGraph_ = std::make_unique<RenderGraph>();

    // Create overlay system (selection circle + fullscreen overlay)
    overlaySystem_ = std::make_unique<OverlaySystem>(vkCtx);
    renderGraph_->registerResource("shadow_depth");
    renderGraph_->registerResource("volumetric_fog");
    renderGraph_->registerResource("reflection_texture");
    renderGraph_->registerResource("scene_color");
    renderGraph_->registerResource("scene_depth");
    renderGraph_->registerResource("final_image");

    LOG_INFO("Renderer initialized");
    return true;
}

void Renderer::shutdown() {
    syncWorldRecording();
    // A recording in progress is finished, not abandoned: the file is only
    // playable once its index is written.
    if (recorder_) stopRecording();

    destroySecondaryCommandResources();

    LOG_DEBUG("Renderer::shutdown - terrainManager stopWorkers...");
    if (terrainManager) {
        terrainManager->stopWorkers();
        LOG_DEBUG("Renderer::shutdown - terrainManager reset...");
        terrainManager.reset();
    }

    LOG_DEBUG("Renderer::shutdown - terrainRenderer...");
    if (terrainRenderer) {
        terrainRenderer->shutdown();
        terrainRenderer.reset();
    }

    LOG_DEBUG("Renderer::shutdown - waterRenderer...");
    if (waterRenderer) {
        waterRenderer->shutdown();
        waterRenderer.reset();
    }

    LOG_DEBUG("Renderer::shutdown - minimap...");
    if (minimap) {
        minimap->shutdown();
        minimap.reset();
    }

    LOG_DEBUG("Renderer::shutdown - worldMap...");
    if (worldMap) {
        worldMap->shutdown();
        worldMap.reset();
    }

    if (sunShafts_) {
        sunShafts_->shutdown();
        sunShafts_.reset();
    }
    if (screenEffects_) {
        screenEffects_->shutdown();
        screenEffects_.reset();
    }

    LOG_DEBUG("Renderer::shutdown - skySystem...");
    if (skySystem) {
        skySystem->shutdown();
        skySystem.reset();
    }

    // Individual sky components are owned by skySystem; just null the aliases
    skybox = nullptr;
    celestial = nullptr;
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    if (weather) {
        weather.reset();
    }

    if (lightning) {
        lightning->shutdown();
        lightning.reset();
    }

    if (swimEffects) {
        swimEffects->shutdown();
        swimEffects.reset();
    }

    if (blobShadowRenderer) {
        blobShadowRenderer->shutdown();
        blobShadowRenderer.reset();
    }
    if (footprintRenderer) {
        footprintRenderer->shutdown();
        footprintRenderer.reset();
    }
    if (fishingLines_) {
        fishingLines_->shutdown();
        fishingLines_.reset();
    }

    LOG_DEBUG("Renderer::shutdown - characterRenderer...");
    if (characterRenderer) {
        characterRenderer->shutdown();
        characterRenderer.reset();
    }

    // Shutdown AnimationController before renderers it references (§4.2)
    animationController_.reset();

    LOG_DEBUG("Renderer::shutdown - wmoRenderer...");
    if (wmoRenderer) {
        wmoRenderer->shutdown();
        wmoRenderer.reset();
    }

    // Shutdown SpellVisualSystem before M2Renderer (it holds M2Renderer pointer) (§4.4)
    if (spellVisualSystem_) {
        spellVisualSystem_->shutdown();
        spellVisualSystem_.reset();
    }

    if (grassRenderer_) {
        // The whole session in one line, at the end where the bounded log
        // cannot rotate it away before anyone reads it.
        if (grassRebuilds_ > 0) {
            LOG_INFO("Grass session: ", grassRebuilds_, " rebuilds, last population ",
                     grassLastCount_, " blades, ", grassProfiles_.size(),
                     " profiles derived, worst generate ",
                     static_cast<int>(grassWorstGenerateMs_), "ms");
        }
        grassRenderer_->shutdown();
        grassRenderer_.reset();
    }

    LOG_DEBUG("Renderer::shutdown - m2Renderer...");
    if (hizSystem_) {
        hizSystem_->shutdown();
        hizSystem_.reset();
    }
    if (m2Renderer) {
        m2Renderer->shutdown();
        m2Renderer.reset();
    }
    if (skyboxModelRenderer_) {
        skyboxModelRenderer_->shutdown();
        skyboxModelRenderer_.reset();
        skyLayers_.clear();
        loadedSkyModels_.clear();
    }
    if (starsModelRenderer_) {
        starsModelRenderer_->shutdown();
        starsModelRenderer_.reset();
        starsInstanceId_ = 0;
        starsModelTried_ = false;
        starsAlpha_ = 0.0f;
    }

    // Audio shutdown is handled by AudioCoordinator (owned by Application).
    audioCoordinator_ = nullptr;

    // Cleanup selection circle + overlay resources
    if (overlaySystem_) {
        overlaySystem_->cleanup();
        overlaySystem_.reset();
    }

    // Shutdown post-process pipeline (FSR/FXAA/FSR2 resources) (§4.3)
    if (postProcessPipeline_) {
        postProcessPipeline_->shutdown();
        postProcessPipeline_.reset();
    }

    // Destroy render graph
    renderGraph_.reset();

    destroyPerFrameResources();

    zoneManager.reset();

    performanceHUD.reset();
    cameraController.reset();
    camera.reset();

    LOG_INFO("Renderer shutdown");
}

void Renderer::registerPreview(CharacterPreview* preview) {
    if (!preview) return;
    auto it = std::find(activePreviews_.begin(), activePreviews_.end(), preview);
    if (it == activePreviews_.end()) {
        activePreviews_.push_back(preview);
    }
}

void Renderer::unregisterPreview(CharacterPreview* preview) {
    auto it = std::find(activePreviews_.begin(), activePreviews_.end(), preview);
    if (it != activePreviews_.end()) {
        activePreviews_.erase(it);
    }
}

void Renderer::setEnhancedWaterEnabled(bool enabled) {
    // Refraction belongs to the enhanced water: its shoreline, meniscus and
    // underwater tint are written against water that refracts, so it is on
    // exactly when that water is drawn. The client's own liquid needs neither
    // the scene copy nor the reflection pass.
    enhancedWater_ = enabled;
    if (waterRenderer) {
        waterRenderer->setEnhancedWater(enabled);
        waterRenderer->setRefractionEnabled(enabled);
    }
}
void Renderer::setMsaaSamples(VkSampleCountFlagBits samples) {
    if (!vkCtx) return;

    // Only a device that cannot resolve depth has to choose between the
    // temporal upscaler and multisampling; everywhere else both run. The
    // saved choice is kept either way and re-asserted when the upscaler goes
    // off, by the settings panel.
    if (postProcessPipeline_ && postProcessPipeline_->isFsr2BlockingMsaa() && samples > VK_SAMPLE_COUNT_1_BIT) {
        LOG_WARNING("Multisampling left off: the temporal upscaler is on and this device "
                    "cannot resolve depth");
        return;
    }

    // Clamp to device maximum
    VkSampleCountFlagBits maxSamples = vkCtx->getMaxUsableSampleCount();
    if (samples > maxSamples) samples = maxSamples;

    if (samples == vkCtx->getMsaaSamples()) return;

    // Defer to between frames - cannot destroy render pass/framebuffers mid-frame
    pendingMsaaSamples_ = samples;
    msaaChangePending_ = true;
}

void Renderer::applyMsaaChange() {
    VkSampleCountFlagBits samples = pendingMsaaSamples_;
    msaaChangePending_ = false;

    // Only where the upscaler cannot share a multisampled scene - a device
    // with no depth resolve - does a queued change have to fall back to 1x.
    if (samples > VK_SAMPLE_COUNT_1_BIT &&
        postProcessPipeline_ && postProcessPipeline_->isFsr2BlockingMsaa()) {
        samples = VK_SAMPLE_COUNT_1_BIT;
    }

    VkSampleCountFlagBits current = vkCtx->getMsaaSamples();
    if (samples == current) return;

    // Single GPU wait - all subsequent operations are CPU-side object creation
    vkDeviceWaitIdle(vkCtx->getDevice());

    // Set new MSAA and recreate swapchain (render pass, depth, MSAA image, framebuffers)
    vkCtx->setMsaaSamples(samples);
    if (!vkCtx->recreateSwapchain(window->getDrawableWidth(), window->getDrawableHeight())) {
        LOG_ERROR("MSAA change failed - reverting to 1x");
        vkCtx->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
        (void)vkCtx->recreateSwapchain(window->getDrawableWidth(), window->getDrawableHeight());
    }

    // Recreate all sub-renderer pipelines (they embed sample count from render pass)
    if (terrainRenderer) terrainRenderer->recreatePipelines();
    if (grassRenderer_) grassRenderer_->recreatePipelines();
    if (waterRenderer) {
        waterRenderer->recreatePipelines();
        // Under MSAA the water draws single-sampled in its own pass, after the
        // scene has resolved - it is a large alpha-blended surface whose edges
        // MSAA does nothing for, and drawing it there also keeps it out of its
        // own refraction copy.
        waterRenderer->destroyWater1xResources();
        setupWater1xPass();
    }
    if (wmoRenderer) wmoRenderer->recreatePipelines();
    if (m2Renderer) m2Renderer->recreatePipelines();
    if (skyboxModelRenderer_) skyboxModelRenderer_->recreatePipelines();
    if (starsModelRenderer_) starsModelRenderer_->recreatePipelines();
    if (characterRenderer) characterRenderer->recreatePipelines();
    if (questMarkerRenderer) questMarkerRenderer->recreatePipelines();
    if (footprintRenderer) footprintRenderer->recreatePipelines();
    if (blobShadowRenderer) blobShadowRenderer->recreatePipelines();
    if (weather) weather->recreatePipelines();
    if (lightning) lightning->recreatePipelines();
    if (swimEffects) {
        syncSwimEffectsTargetPass();
        swimEffects->recreatePipelines();
    }
    if (mountDust) mountDust->recreatePipelines();
    if (chargeEffect) chargeEffect->recreatePipelines();
    if (fishingLines_) fishingLines_->recreatePipelines();

    // Sky system sub-renderers
    if (skySystem) {
        if (auto* sb = skySystem->getSkybox()) sb->recreatePipelines();
        if (auto* sf = skySystem->getStarField()) sf->recreatePipelines();
        if (auto* ce = skySystem->getCelestial()) ce->recreatePipelines();
        if (auto* cl = skySystem->getClouds()) cl->recreatePipelines();
        if (auto* lf = skySystem->getLensFlare()) lf->recreatePipelines();
    }

    if (minimap) {
        // After syncSwimEffectsTargetPass above, which is what decides the pass
        // this is built against.
        minimap->recreatePipelines();
    }

    // Resize HiZ pyramid (depth format/MSAA may have changed)
    if (hizSystem_) {
        auto ext = vkCtx->getSwapchainExtent();
        if (!hizSystem_->resize(ext.width, ext.height)) {
            LOG_WARNING("HiZ resize failed after MSAA change");
            if (m2Renderer) m2Renderer->setHiZSystem(nullptr);
            hizSystem_->shutdown();
            hizSystem_.reset();
        }
    }

    // Selection circle + overlay + FSR use lazy init, just destroy them
    if (overlaySystem_) overlaySystem_->recreatePipelines();
    if (postProcessPipeline_) postProcessPipeline_->destroyAllResources(); // Will be lazily recreated in beginFrame()

    // ImGui is deliberately not restarted here.
    //
    // It always initialises at one sample into the overlay pass, which is
    // itself always single-sampled and depends only on the swapchain format -
    // so a change of scene anti-aliasing does not change anything ImGui built.
    // Recreating the swapchain produces a new overlay pass handle, but a
    // structurally identical one, and Vulkan requires a pipeline's render pass
    // to be compatible rather than the same object.
    //
    // Tearing the backend down destroyed its descriptor pool, and every UI
    // texture in the client - item and spell icons, raid icons, the talent
    // background, the world map layers, the widget renderer - holds a
    // descriptor set allocated from it. Nothing was told, so the next frame
    // drew with freed descriptors and the GPU was reset: the log shows the
    // swapchain and pipelines rebuilt, then the fence wait failing with
    // VK_ERROR_DEVICE_LOST a fraction of a second later. Applying a saved
    // anti-aliasing setting at startup made that look like a crash on launch.

}

void Renderer::beginFrame() {
    ZoneScopedN("Renderer::beginFrame");
    // A world still pending here belongs to a frame that never reached
    // endFrame - an exception out of the interface, say. Its command buffer
    // is not going to be submitted; only its worker has to be waited for.
    if (pendingWorld_.pending) {
        LOG_WARNING("renderWorld was never finished; dropping its recording");
        pendingWorld_.pending = false;
    }
    // The same for a shadow pass planned and never recorded: its prologue is
    // reset with the rest of the slot below.
    shadowPlan_.pending = false;
    syncWorldRecording();
    if (!vkCtx) return;
    if (vkCtx->isDeviceLost()) return;

    // The ablation's clock. Top of one frame to the top of the next is the
    // frame's whole wall time, which is the number a pass has to move to be
    // worth anything - a pass that gets cheaper while the frame does not has
    // not been paid for.
    if (passAblation_) {
        const auto now = std::chrono::steady_clock::now();
        // Only frames that drew the world. A frame of character select costs
        // five milliseconds and a frame with a zone streaming into it costs a
        // hundred and twenty, and the first run of this walked its baseline
        // through the former and its terrain phase through the latter - it
        // reported that terrain was worth minus 110 milliseconds.
        if (worldDrawnLastFrame_ && lastFrameStart_.time_since_epoch().count() != 0) {
            const bool wasSettling = passAblation_->settling();
            const AblationPass before = passAblation_->current();
            passAblation_->frame(std::chrono::duration<double, std::milli>(
                now - lastFrameStart_).count());
            // Say where it has got to. A run is the better part of a minute of
            // standing still and the first one silently went eight phases deep
            // before it was quit, with no way to tell it had started.
            if (passAblation_->running() &&
                (passAblation_->current() != before || (wasSettling && !passAblation_->settling()))) {
                LOG_WARNING("Pass ablation ", passAblation_->phaseNumber(), "/",
                            PassAblation::phaseCount(), ": ",
                            ablationPassName(passAblation_->current()),
                            passAblation_->current() == AblationPass::None ? "" : " off");
            }
        }
        lastFrameStart_ = now;
        worldDrawnLastFrame_ = false;
        if (!passAblation_->running() && !passAblationReported_) {
            passAblationReported_ = true;
            LOG_WARNING(passAblation_->report());
        }
    }

    worldDrawnThisFrame_ = false;

    // Nothing is drawn while the window has no area, and nothing sized from
    // the swapchain is rebuilt. A minimised window on Windows has a 0x0
    // surface, and rebuilding at that size failed every target from the depth
    // buffer to FXAA's scene colour and then faulted on the next frame. The
    // frame is skipped instead - currentCmd stays null, which every recording
    // path already treats as "no frame", the same as an out-of-date acquire -
    // while the caller keeps pumping events, so the window can come back.
    // Anything queued (an MSAA change, a swapchain marked dirty) waits for it.
    // After the ablation's clock, so the time spent minimised is not counted
    // against the first frame drawn afterwards.
    if (!window->hasDrawableArea()) return;

    // Apply deferred MSAA change between frames (before any rendering state is used)
    // Not onto a surface with no area: the rebuild would refuse it, and a
    // refused rebuild reads to applyMsaaChange as the change having failed -
    // which reverts the player's setting to 1x. Left pending, it applies when
    // the surface comes back.
    if (msaaChangePending_ && vkCtx->surfaceHasArea()) {
        applyMsaaChange();
        // The rebuild destroys and remakes the swapchain, every render pass and
        // every pipeline. The frame slots are left mid-cycle by it, and the
        // next frame would reset a fence and re-record a command buffer the
        // GPU has not finished with - which is what validation reports and the
        // driver answers by losing the device.
        if (vkCtx) vkCtx->resetFrameSyncState();
    }

    // A shadow resolution change remakes the maps the per-frame sets and the
    // fog's sets bind. Between frames for the same reason as the fog below,
    // and before it, so a fog rebuild in the same frame binds the new views.
    applyPendingShadowMapSize();

    // A fog quality change builds or frees its volumes, which the per-frame
    // sets bind - so between frames, before this one's set is used.
    if (volumetricFog_ && volumetricFog_->applyPendingQuality()) writeFogVolumeBindings();

    // Retire finished upload batches every frame.
    //
    // This was polled only from the terrain manager, so batches submitted by
    // anything else retired only while terrain happened to be streaming. A
    // rebuild reported 1423 submitted against 1241 retired - 182 outstanding,
    // each holding a fence, a command buffer and its staging buffers. With
    // FrameXML uploading hundreds of textures the backlog is much larger than
    // it was, and nothing bounded it.
    if (vkCtx) vkCtx->pollUploadBatches();

    // Post-process resource management (§4.3 - delegates to PostProcessPipeline)
    if (postProcessPipeline_) postProcessPipeline_->manageResources();

    // The target that call may have just created or destroyed decides where
    // the water draws, and with it where the spray and the minimap go. Asked
    // again here, after the answer can have changed, rather than only at the
    // anti-aliasing rebuild that runs before the target exists.
    refreshSwimEffectsPass();

    // Handle swapchain recreation if needed
    if (vkCtx->isSwapchainDirty()) {
        // Skip recreation while window is minimized (0×0 extent is a Vulkan spec violation)
        // Pixels, like the extent below: a window with a surface behind it is
        // never rebuilt from the size the desktop places it at.
        //
        // The drawable size holds the last real size through a minimise, so
        // it never answers zero here; the surface is asked as well, because
        // it can be 0x0 while SDL still reports a size - a window something
        // has hidden for a moment, as a screenshot tool does.
        if (window->getDrawableWidth() == 0 || window->getDrawableHeight() == 0) return;
        if (!vkCtx->surfaceHasArea()) return;
        // Nothing below is rebuilt against a swapchain that did not come back.
        // It stays dirty and the next frame tries again; resizing the post
        // effects at a size that failed is what disabled FXAA for good.
        if (!vkCtx->recreateSwapchain(window->getDrawableWidth(), window->getDrawableHeight())) return;
        // Rebuild water resources that reference swapchain extent/views
        if (waterRenderer) {
            waterRenderer->recreatePipelines();
            waterRenderer->destroyWater1xResources();
            setupWater1xPass();
        }
        // Recreate post-process resources for new swapchain dimensions
        if (postProcessPipeline_) postProcessPipeline_->handleSwapchainResize();
        // Resize HiZ depth pyramid for new swapchain dimensions
        if (hizSystem_) {
            auto ext = vkCtx->getSwapchainExtent();
            if (!hizSystem_->resize(ext.width, ext.height)) {
                LOG_WARNING("HiZ resize failed - disabling occlusion culling");
                if (m2Renderer) m2Renderer->setHiZSystem(nullptr);
                hizSystem_->shutdown();
                hizSystem_.reset();
            }
        }
    }

    // Between frames, so a resize of the ray traced lighting's images can wait
    // for the device and rewrite both slots' sets.
    if (rtLighting_ && rtLighting_->prepare(sceneRenderExtent())) writeRtLightingBindings();

    rtRecordedThisFrame_ = false;

    // Acquire swapchain image and begin command buffer
    currentCmd = vkCtx->beginFrame(currentImageIndex);
    if (currentCmd == VK_NULL_HANDLE) {
        // Swapchain out of date, will retry next frame
        return;
    }

    // This slot's fence has just been waited on, so a frame it copied for the
    // recording two frames ago is complete.
    collectRecordedFrame();

    // FSR2 jitter pattern (§4.3 - delegates to PostProcessPipeline)
    if (postProcessPipeline_ && camera) postProcessPipeline_->applyJitter(camera.get());

    // Compute fresh shadow matrix BEFORE UBO update so shaders get current-frame data.
    lightSpaceMatrix = computeLightSpaceMatrix();

    // Update per-frame UBO with current camera/lighting state
    updatePerFrameUBO();

    // The shadow pass, planned now and recorded later into the prologue (see
    // planShadowPass). Before anything else marks the frame: its timestamps
    // are reserved here so that they read back in the order they execute,
    // and the prologue executes ahead of everything this buffer records.
    planShadowPass();

    // ── Early compute: M2 frustum culling ──
    // beginFrame() has already waited for this frame slot's previous fence, so
    // its mapped visibility output is complete and safe for the CPU to reuse.
    // Read/invalidate that completed output, then record the next cull dispatch
    // directly into the normal frame command buffer. The old path submitted a
    // separate command buffer and synchronously waited on a fence every frame,
    // serializing CPU and GPU work solely to obtain same-frame cull results.
    if (m2Renderer && camera && vkCtx) {
        uint32_t frame = vkCtx->getCurrentFrame();
        m2Renderer->invalidateCullOutput(frame);
        m2Renderer->dispatchCullCompute(currentCmd, frame, *camera);
        vkCtx->gpuMark(currentCmd, "m2 cull (compute)");
    }

    // Grass culls here too, for the same reason: a dispatch has to be recorded
    // outside a render pass. Unlike the M2 path nothing reads the result back -
    // the count it produces is consumed by the indirect draw on the GPU.
    if (grassRenderer_ && camera && vkCtx) {
        updateGrassPopulation();
        grassRenderer_->reportCullResult();
        grassRenderer_->dispatchCull(currentCmd, vkCtx->getCurrentFrame(), *camera,
                                     characterPosition);
        vkCtx->gpuMark(currentCmd, "grass cull (compute)");
    }

    // --- Off-screen pre-passes ---
    // Build frame graph: registers pre-passes as graph nodes with dependencies.
    // compile() topologically sorts; execute() runs them with auto barriers.
    buildFrameGraph(nullptr);
    if (renderGraph_) {
        renderGraph_->execute(currentCmd);
    }

    // --- Begin render pass ---
    // Select framebuffer: PP off-screen target or swapchain (§4.3 - PostProcessPipeline)
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = vkCtx->getImGuiRenderPass();

    VkExtent2D renderExtent;
    VkFramebuffer ppFB = postProcessPipeline_ ? postProcessPipeline_->getSceneFramebuffer() : VK_NULL_HANDLE;
    if (ppFB != VK_NULL_HANDLE) {
        rpInfo.framebuffer = ppFB;
        renderExtent = postProcessPipeline_->getSceneRenderExtent();
    } else {
        rpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[currentImageIndex];
        renderExtent = vkCtx->getSwapchainExtent();
    }

    rpInfo.renderArea.offset = {.x = 0, .y = 0};
    rpInfo.renderArea.extent = renderExtent;

    // Clear values must match attachment count: 2 (no MSAA), 3 (MSAA), or 4 (MSAA+depth resolve)
    VkClearValue clearValues[4]{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {.depth = 1.0f, .stencil = 0};
    clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[3].depthStencil = {.depth = 1.0f, .stencil = 0};
    bool msaaOn = (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT);
    if (msaaOn) {
        bool depthRes = (vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE);
        rpInfo.clearValueCount = depthRes ? 4 : 3;
    } else {
        rpInfo.clearValueCount = 2;
    }
    rpInfo.pClearValues = clearValues;

    // Cache render pass state for secondary command buffer inheritance
    activeRenderPass_ = rpInfo.renderPass;
    activeFramebuffer_ = rpInfo.framebuffer;
    activeRenderExtent_ = renderExtent;

    VkSubpassContents subpassMode = parallelRecordingEnabled_
        ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(currentCmd, &rpInfo, subpassMode);

    if (!parallelRecordingEnabled_) {
        // Fallback: set dynamic viewport and scissor on primary (inline mode)
        VkViewport viewport{};
        viewport.width = static_cast<float>(renderExtent.width);
        viewport.height = static_cast<float>(renderExtent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = renderExtent;
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);
    }
}

void Renderer::endFrame() {
    ZoneScopedN("Renderer::endFrame");
    // Normally already done by the caller, which times it as its own stage.
    // It joins the shadow worker too.
    finishRenderWorld();
    if (!vkCtx || currentCmd == VK_NULL_HANDLE) {
        shadowPlan_.pending = false;
        return;
    }
    // A shadow pass no worker took - no world this frame, or the world drawn
    // inline - is recorded here, before the prologue it goes into is ended.
    flushShadowPass();

    logViewDistanceDiag();

    // Post-process execution (§4.3 - delegates to PostProcessPipeline). Whether
    // it swapped the scene pass for an INLINE one no longer matters to the
    // caller: the UI is drawn in the overlay pass, which this function opens
    // itself once whichever pass is current has been closed.
    bool postProcessed = false;
    if (postProcessPipeline_) {
        // True when it closed the scene pass and opened its own output pass,
        // which is when there is something of its own to mark - and a pass
        // the primary may record into. Otherwise the scene pass, perhaps one
        // that takes only secondaries, is still open.
        if (postProcessPipeline_->executePostProcessing(
                currentCmd, currentImageIndex, camera.get(), lastDeltaTime_)) {
            postProcessed = true;
            if (vkCtx) vkCtx->gpuMark(currentCmd, "post-process output (FXAA / FSR / sharpen)");
        }
    }

    // The scene is complete: close its pass so the water refraction copy can run
    // (a copy is illegal inside a render pass), then draw the UI in the overlay
    // pass. Capturing after the UI instead is what refracted the interface into
    // the water. The overlay pass is single-sampled and colour-only, which is
    // also why the UI costs the same here whatever MSAA the scene uses.
    vkCmdEndRenderPass(currentCmd);
    // The scene's own store and resolve, when no post-processing took the
    // pass over and marked its end itself.
    if (vkCtx && !postProcessed) vkCtx->gpuMark(currentCmd, "scene pass end (store / resolve)");

    // Only when water could not be moved out of the scene pass (MSAA). Otherwise
    // renderWorld already took the copy at the one point in the frame where the
    // scene is finished but the water is not yet over it; copying again here
    // would replace that with an image containing the water.
    if (!waterDrawsInContinuePass()
        && waterRenderer && waterRenderer->isRefractionEnabled() && waterRenderer->hasSurfaces()
        && currentImageIndex < vkCtx->getSwapchainImages().size()) {
        // The depth is the scene's, wherever the scene went. With MSAA and FXAA
        // or FSR the scene renders into the post-processing target and nothing
        // in the frame touches the context's own depth: this copied that image,
        // never written, into the water's depth - which is what its absorption
        // and its shoreline foam are measured against - and validation caught
        // the first frame reading it from UNDEFINED.
        VkImage depthSrc = vkCtx->getDepthCopySourceImage();
        bool depthIsMsaa = vkCtx->isDepthCopySourceMsaa();
        VkExtent2D depthExtent = vkCtx->getSwapchainExtent();
        if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
            depthSrc = postProcessPipeline_->getSceneDepthImage();
            depthIsMsaa = postProcessPipeline_->sceneDepthIsMsaa();
            depthExtent = postProcessPipeline_->getSceneRenderExtent();
        }
        waterRenderer->captureSceneHistory(
            currentCmd,
            vkCtx->getSwapchainImages()[currentImageIndex],
            depthSrc,
            vkCtx->getSwapchainExtent(),
            depthIsMsaa,
            vkCtx->getCurrentFrame(),
            depthExtent);
        vkCtx->gpuMark(currentCmd, "water refraction copy");
    }

    // After the scene's passes, before the overlay pass that shows them.
    recordPreviewComposites();

    // The picture is finished and out of every pass that drew it: the one
    // point where the shafts can copy it down, before the overlay pass opens.
    recordScreenEffects();
    if (vkCtx) vkCtx->gpuMark(currentCmd, "screen effects");
    recordSunShafts();
    if (vkCtx) vkCtx->gpuMark(currentCmd, "sun shafts");

    const auto& overlayFbs = vkCtx->getOverlayFramebuffers();
    if (vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE && currentImageIndex < overlayFbs.size()) {
        VkRenderPassBeginInfo overlayRp{};
        overlayRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        overlayRp.renderPass = vkCtx->getOverlayRenderPass();
        overlayRp.framebuffer = overlayFbs[currentImageIndex];
        overlayRp.renderArea.extent = vkCtx->getSwapchainExtent();
        vkCmdBeginRenderPass(currentCmd, &overlayRp, VK_SUBPASS_CONTENTS_INLINE);

        VkExtent2D ext = vkCtx->getSwapchainExtent();
        VkViewport vp{};
        vp.width = static_cast<float>(ext.width);
        vp.height = static_cast<float>(ext.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = ext;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        // Under the interface, over everything else: the frame redrawn with
        // the glow and death passes, then the shafts screened over it.
        if (screenEffects_) screenEffects_->composite(currentCmd, vkCtx->getCurrentFrame());
        if (sunShafts_) sunShafts_->composite(currentCmd, vkCtx->getCurrentFrame());
        vkCtx->gpuMark(currentCmd, "effects composite");

        // ImGui's pipelines are built against the overlay pass, so it always
        // records inline here rather than into a scene-pass secondary buffer.
        {
            WOWEE_PROFILE_SCOPE("record interface (ImGui)", Cpu);
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), currentCmd);
        }
        vkCmdEndRenderPass(currentCmd);
        if (vkCtx) vkCtx->gpuMark(currentCmd, "interface (ImGui)");
    } else {
        LOG_ERROR("Overlay render pass missing - UI not drawn this frame");
    }

    // Water now renders in the main pass (renderWorld), no separate 1x pass needed.

    if (afterInterface_) afterInterface_(currentCmd);

    // Last, so the recording holds everything the player sees - and then the
    // recording dot, which it does not.
    recordScreenCapture();
    if (afterInterface_ || recorder_) vkCtx->gpuMark(currentCmd, "capture / after interface");

    // Submit and present
    vkCtx->endFrame(currentCmd, currentImageIndex);
    currentCmd = VK_NULL_HANDLE;
}

void Renderer::setCharacterFollow(uint32_t instanceId) {
    characterInstanceId = instanceId;
    if (cameraController && instanceId > 0) {
        cameraController->setFollowTarget(&characterPosition);
    }
    if (animationController_) animationController_->onCharacterFollow(instanceId);
}

bool Renderer::startRecording(const std::string& path, std::string& error) {
    if (recorder_ && recorder_->isRecording()) {
        error = "already recording";
        return false;
    }
    if (!vkCtx) {
        error = "there is no renderer to record from";
        return false;
    }
    if (!core::ScreenRecorder::compiledIn()) {
        error = "this build was made without FFmpeg 5.1 or later, which recording needs";
        return false;
    }
    const VkExtent2D extent = vkCtx->getSwapchainExtent();
    const core::RecordingSize size = core::recordingFrameSize(extent.width, extent.height);
    auto capture = std::make_unique<ScreenCapture>();
    if (!capture->initialize(vkCtx, size.width, size.height)) {
        capture->shutdown();
        error = "could not set up reading frames back from the GPU";
        return false;
    }
    auto recorder = std::make_unique<core::ScreenRecorder>();
    if (!recorder->start(path, size, error)) {
        capture->shutdown();
        return false;
    }
    screenCapture_ = std::move(capture);
    recorder_ = std::move(recorder);
    recordingFailure_.clear();
    return true;
}

core::ScreenRecorder::Stats Renderer::stopRecording() {
    core::ScreenRecorder::Stats stats;
    if (!recorder_) return stats;
    // The frames still on the GPU are finished and handed over, oldest first,
    // so the file ends where the recording did rather than two frames short.
    if (vkCtx && screenCapture_) {
        vkDeviceWaitIdle(vkCtx->getDevice());
        ScreenCapture::Ready ready[2] = {screenCapture_->collect(0), screenCapture_->collect(1)};
        if (ready[0].valid && ready[1].valid && ready[1].pts < ready[0].pts) std::swap(ready[0], ready[1]);
        ScreenCapture* capture = screenCapture_.get();
        for (const auto& r : ready) {
            if (!r.valid) continue;
            const uint32_t buffer = r.buffer;
            recorder_->submit({.bgra = r.bgra, .stride = r.stride, .pts = r.pts,
                               .release = [capture, buffer] { capture->release(buffer); }});
        }
    }
    // The recorder first: its thread holds readback buffers until it is done.
    stats = recorder_->stop();
    recorder_.reset();
    if (screenCapture_) {
        screenCapture_->shutdown();
        screenCapture_.reset();
    }
    return stats;
}

bool Renderer::isRecording() const {
    return recorder_ && recorder_->isRecording();
}

std::string Renderer::takeRecordingFailure() {
    std::string failure;
    failure.swap(recordingFailure_);
    return failure;
}

void Renderer::collectRecordedFrame() {
    if (!recorder_ || !screenCapture_ || !vkCtx) return;
    const ScreenCapture::Ready ready = screenCapture_->collect(vkCtx->getCurrentFrame());
    if (ready.valid) {
        ScreenCapture* capture = screenCapture_.get();
        const uint32_t buffer = ready.buffer;
        recorder_->submit({.bgra = ready.bgra, .stride = ready.stride, .pts = ready.pts,
                           .release = [capture, buffer] { capture->release(buffer); }});
    }
    // Given up on its own: finish what it has and say why, once.
    if (recorder_->failed()) {
        recordingFailure_ = recorder_->failure();
        stopRecording();
    }
}

void Renderer::recordScreenCapture() {
    if (!recorder_ || !screenCapture_ || !recorder_->isRecording() || currentCmd == VK_NULL_HANDLE) return;
    const auto& images = vkCtx->getSwapchainImages();
    const VkExtent2D extent = vkCtx->getSwapchainExtent();
    int64_t pts = 0;
    if (currentImageIndex < images.size() && recorder_->frameDue(&pts)) {
        if (!screenCapture_->record(currentCmd, vkCtx->getCurrentFrame(), images[currentImageIndex],
                                    extent, pts)) {
            recorder_->frameDropped();
        }
        recorder_->frameTaken(pts);
    }
    const auto& overlayFbs = vkCtx->getOverlayFramebuffers();
    if (vkCtx->getOverlayRenderPass() != VK_NULL_HANDLE && currentImageIndex < overlayFbs.size()) {
        screenCapture_->drawIndicator(currentCmd, overlayFbs[currentImageIndex], extent,
                                      static_cast<float>(recorder_->elapsedSeconds()));
    }
}

bool Renderer::captureScreenshot(const std::string& outputPath) {
    if (!vkCtx) return false;

    VkDevice device     = vkCtx->getDevice();
    VmaAllocator alloc  = vkCtx->getAllocator();
    VkExtent2D extent   = vkCtx->getSwapchainExtent();
    const auto& images  = vkCtx->getSwapchainImages();

    if (images.empty() || currentImageIndex >= images.size()) return false;

    VkImage srcImage = images[currentImageIndex];
    uint32_t w = extent.width;
    uint32_t h = extent.height;
    VkDeviceSize bufSize = static_cast<VkDeviceSize>(w) * h * 4;

    // Stall GPU so the swapchain image is idle
    vkDeviceWaitIdle(device);

    // Create staging buffer
    VkBufferCreateInfo bufInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size  = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (vmaCreateBuffer(alloc, &bufInfo, &allocCI, &stagingBuf, &stagingAlloc, nullptr) != VK_SUCCESS) {
        LOG_WARNING("Screenshot: failed to create staging buffer");
        return false;
    }

    // Record copy commands
    VkCommandBuffer cmd = vkCtx->beginSingleTimeCommands();

    // Transition swapchain image: PRESENT_SRC → TRANSFER_SRC
    VkImageMemoryBarrier2 toTransfer{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toTransfer.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.image               = srcImage;
    toTransfer.subresourceRange    = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    VkDependencyInfo toTransferDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toTransferDep.dependencyFlags = 0;
    toTransferDep.imageMemoryBarrierCount = 1;
    toTransferDep.pImageMemoryBarriers = &toTransfer;
    cmdPipelineBarrier2(cmd, toTransferDep);

    // Copy image to buffer
    VkBufferImageCopy region{};
    region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    region.imageExtent      = {.width = w, .height = h, .depth = 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition back: TRANSFER_SRC → PRESENT_SRC
    VkImageMemoryBarrier2 toPresent = toTransfer;
    toPresent.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toPresent.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkDependencyInfo toPresentDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    toPresentDep.imageMemoryBarrierCount = 1;
    toPresentDep.pImageMemoryBarriers = &toPresent;
    cmdPipelineBarrier2(cmd, toPresentDep);

    vkCtx->endSingleTimeCommands(cmd);

    // Map and convert BGRA → RGBA
    void* mapped = nullptr;
    vmaMapMemory(alloc, stagingAlloc, &mapped);
    auto* pixels = static_cast<uint8_t*>(mapped);
    for (uint32_t i = 0; i < w * h; ++i) {
        std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]); // B ↔ R
    }

    // Ensure output directory exists
    std::filesystem::path outPath(outputPath);
    if (outPath.has_parent_path())
        std::filesystem::create_directories(outPath.parent_path());

    int ok = stbi_write_png(outputPath.c_str(),
                            static_cast<int>(w), static_cast<int>(h),
                            4, pixels, static_cast<int>(w * 4));

    vmaUnmapMemory(alloc, stagingAlloc);
    vmaDestroyBuffer(alloc, stagingBuf, stagingAlloc);

    if (ok) {
        LOG_INFO("Screenshot saved: ", outputPath);
    } else {
        LOG_WARNING("Screenshot: stbi_write_png failed for ", outputPath);
    }
    return ok != 0;
}

void Renderer::resetCombatVisualState() {
    if (animationController_) animationController_->resetCombatVisualState();
    if (spellVisualSystem_) spellVisualSystem_->reset();
}

const std::string& Renderer::getCurrentZoneName() const {
    static const std::string empty;
    return audioCoordinator_ ? audioCoordinator_->getCurrentZoneName() : empty;
}

bool Renderer::updateSkyboxLayers() {
    // Which skybox models a place uses is Light.dbc's answer, not a map id:
    // LightParams names a LightSkybox row and LightSkybox names the model.
    // LightingManager walks that for the lights around the player and says
    // how much of each is up; this keeps one instance per model and fades it
    // by that weight, so crossing from one zone's sky to another's is a blend
    // and not a swap. WOWEE_NO_SKY_M2=1 draws the procedural sky alone.
    static const bool noSkyM2 = std::getenv("WOWEE_NO_SKY_M2") != nullptr;
    if (noSkyM2) return false;
    if (!skyboxModelRenderer_ || !lightingManager || !cachedAssetManager || !camera) {
        return false;
    }

    auto normalized = [](std::string p) {
        std::replace(p.begin(), p.end(), '/', '\\');
        return p;
    };
    const auto& layers = lightingManager->getSkyboxLayers();

    // Faded out and no longer wanted.
    std::erase_if(skyLayers_, [&](const SkyLayerInstance& sky) {
        const bool wanted = std::any_of(layers.begin(), layers.end(), [&](const auto& l) {
            return normalized(l.path) == sky.path;
        });
        if (!wanted) skyboxModelRenderer_->removeInstance(sky.instanceId);
        return !wanted;
    });

    for (const auto& layer : layers) {
        const std::string path = normalized(layer.path);
        auto it = std::find_if(skyLayers_.begin(), skyLayers_.end(),
                               [&](const SkyLayerInstance& s) { return s.path == path; });
        if (it == skyLayers_.end()) {
            const uint32_t modelId = loadSkyboxModel(path);
            if (modelId == 0) continue;
            const uint32_t instanceId = skyboxModelRenderer_->createInstance(
                modelId, camera->getPosition(), glm::vec3(0.0f), 1.0f);
            if (instanceId == 0) continue;
            skyboxModelRenderer_->setSkipCollision(instanceId, true);
            skyLayers_.push_back({.path = path, .instanceId = instanceId});
            it = std::prev(skyLayers_.end());
        }
        skyboxModelRenderer_->setInstanceFade(it->instanceId, layer.weight);
        skyboxModelRenderer_->setInstancePosition(it->instanceId, camera->getPosition());
        // Flag 0x1: the model's animation is the time of day, game minutes
        // over 1440 of its length (0x007ecf20), not a clock of its own.
        if (layer.flags & LightingManager::kSkyboxFlagTimeOfDayAnim) {
            skyboxModelRenderer_->setInstanceAnimationFraction(
                it->instanceId, lightingManager->getTimeOfDay());
        }
    }
    return !skyLayers_.empty();
}

uint32_t Renderer::loadSkyboxModel(const std::string& path) {
    if (auto it = loadedSkyModels_.find(path); it != loadedSkyModels_.end()) return it->second;
    if (failedSkyboxPaths_.count(path)) return 0;
    const uint32_t modelId = uploadSkyModel(*skyboxModelRenderer_, path);
    if (modelId == 0) {
        failedSkyboxPaths_.insert(path);
        return 0;
    }
    loadedSkyModels_[path] = modelId;
    return modelId;
}

uint32_t Renderer::uploadSkyModel(M2Renderer& target, const std::string& path) {
    std::vector<std::string> candidates{path};
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        candidates.push_back(path + ".m2");
    } else {
        std::string ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".mdx" || ext == ".mdl") candidates.push_back(path.substr(0, dot) + ".m2");
    }

    std::vector<uint8_t> modelData;
    std::string resolvedPath;
    for (const auto& candidate : candidates) {
        modelData = cachedAssetManager->readFileOptional(candidate);
        if (!modelData.empty()) {
            resolvedPath = candidate;
            break;
        }
    }
    if (modelData.empty()) {
        LOG_WARNING("Sky model unavailable: ", path);
        return 0;
    }

    pipeline::M2Model model = pipeline::M2Loader::load(modelData);
    model.name = resolvedPath + "#original-sky";
    const std::string skinPath = pipeline::skinPathForM2(resolvedPath);
    auto skinData = cachedAssetManager->readFileOptional(skinPath);
    if (!skinData.empty() && model.version >= 264) {
        pipeline::M2Loader::loadSkin(skinData, model);
    }
    if (!model.isValid()) {
        LOG_WARNING("Sky model is invalid: ", resolvedPath);
        return 0;
    }

    const uint32_t modelId = static_cast<uint32_t>(std::hash<std::string>{}(model.name));
    if (!target.loadModel(model, modelId)) {
        LOG_WARNING("Failed to upload sky model: ", resolvedPath);
        return 0;
    }
    LOG_INFO("Sky model loaded: ", resolvedPath);
    return modelId;
}

void Renderer::updateStarsModel(float deltaTime) {
    // Loaded once, the first frame there is somewhere to put it (0x009abb00
    // loads it with the rest of the sky).
    if (!starsModelRenderer_ || !camera || !cachedAssetManager) return;
    if (!starsModelTried_) {
        starsModelTried_ = true;
        const uint32_t modelId = uploadSkyModel(*starsModelRenderer_, daynight::kStarsModelPath);
        if (modelId != 0) {
            starsInstanceId_ = starsModelRenderer_->createInstance(
                modelId, camera->getPosition(), glm::vec3(0.0f), 1.0f);
            if (starsInstanceId_ != 0) starsModelRenderer_->setSkipCollision(starsInstanceId_, true);
        }
    }
    if (starsInstanceId_ == 0) {
        starsAlpha_ = 0.0f;
        return;
    }
    // At the eye, at the alpha the star clock gives (0x007ee0d0), running on
    // its own clock (0x009abd50 advances it by the frame's milliseconds).
    const float dayFraction = lightingManager ? lightingManager->getTimeOfDay() : 0.5f;
    starsAlpha_ = daynight::starsAlpha(dayFraction);
    starsModelRenderer_->setInstanceFade(starsInstanceId_, starsAlpha_);
    starsModelRenderer_->setInstancePosition(starsInstanceId_, camera->getPosition());
    starsModelRenderer_->update(deltaTime, camera->getPosition(),
                                camera->getProjectionMatrix() * camera->getViewMatrix());
}

bool Renderer::isOnOutdoorPvpObjective() const {
    if (!zoneManager || !terrainManager) return false;
    if (const auto areaId = terrainManager->getAreaIdAt(
            characterPosition.x, characterPosition.y)) {
        return zoneManager->isOutdoorPvpArea(*areaId);
    }
    return false;
}

uint32_t Renderer::getCurrentZoneId() const {
    // A zone remembered from the last map is worse than none: the sticky
    // answer below would hold Duskwood's pinned midnight over a continent
    // away until the first chunk of the new one loaded.
    if (const auto* gh = core::Application::getInstance().getGameHandler()) {
        const uint32_t mapId = gh->getCurrentMapId();
        if (mapId != lastResolvedZoneMapId_) {
            lastResolvedZoneMapId_ = mapId;
            lastResolvedZoneId_ = 0;
        }
    }

    uint32_t tileZoneId = 0;
    if (zoneManager && terrainManager) {
        // The area the player is in as the music and the ambience have it
        // (0x00782560): the building's group the player stands on where its
        // row names one - Dalaran, not the Crystalsong Forest a long way under
        // it - and the chunk under the player otherwise.
        std::optional<uint32_t> areaId;
        if (playerLinkedArea_ != 0) areaId = playerLinkedArea_;
        else areaId = terrainManager->getAreaIdAt(characterPosition.x, characterPosition.y);
        if (areaId) {
            lastResolvedZoneId_ = zoneManager->resolveAreaZoneId(*areaId);
            return lastResolvedZoneId_;
        }
        const auto tile = terrainManager->getCurrentTile();
        tileZoneId = zoneManager->getZoneId(tile.x, tile.y);
    }

    // The chunk under the player did not say which area it is - either its
    // ADT is not resident yet, or its area id is zero, which a great many
    // chunks carry. That is "this chunk does not know", not "the zone
    // changed", and answering from a different source instead made the zone
    // id flip back and forth as the player walked from a chunk that knew to
    // one that did not.
    //
    // Nothing about that is quiet. The zone id picks the dark-zone ambience
    // override, which replaces the four sky colours outright, and in Duskwood
    // it also pins the visual hour to the small hours - so the sky changed
    // brightness every time a chunk boundary was crossed and held still the
    // moment the player did. Keep the last chunk that did know.
    if (lastResolvedZoneId_ != 0) return lastResolvedZoneId_;

    const auto* gh = core::Application::getInstance().getGameHandler();
    if (gh && gh->getWorldStateZoneId() != 0) {
        const uint32_t areaId = gh->getWorldStateZoneId();
        return zoneManager ? zoneManager->resolveAreaZoneId(areaId) : areaId;
    }
    if (audioCoordinator_ && audioCoordinator_->getCurrentZoneId() != 0)
        return audioCoordinator_->getCurrentZoneId();
    return tileZoneId;
}

void Renderer::updatePlayerArea() {
    namespace za = game::zone_area;
    namespace mi = minimap_indoor;
    auto* gh = core::Application::getInstance().getGameHandler();
    playerLinks_.clear();
    playerLinkedArea_ = 0;
    playerLinkIndoors_ = false;
    if (!gh || !gh->isInWorld() || !terrainManager || !zoneManager) return;
    if (gh->getCurrentMapId() != zoneTextsMapId_) {
        zoneTextsMapId_ = gh->getCurrentMapId();
        zoneTextsKnown_ = false;
        zoneTexts_ = {};
    }
    loadWmoAreaRows();
    const mi::AreaRows* rows = wmoAreaRows_.empty() ? nullptr : &wmoAreaRows_;
    const glm::vec3 feet = characterPosition;
    const std::optional<float> groundZ = terrainManager->getHeightAt(feet.x, feet.y);
    // How far down from `startZ` the ground is met, none where it is not met
    // on the way down to `endZ`.
    const auto groundDown = [&](float startZ, float endZ) -> std::optional<float> {
        if (!groundZ || *groundZ > startZ || *groundZ < endZ) return std::nullopt;
        return startZ - *groundZ;
    };

    // The groups the player is linked to (0x007c2a70): the group of the first
    // face under the feet that collides, then of the first that is drawn,
    // from a tenth of a yard over them to a thousand under - unless the
    // ground is met first, 0x007c28f0 dropping a building's face the ground's
    // is nearer than.
    std::optional<WMORenderer::GroupArea> linked;
    if (wmoRenderer) {
        const float startZ = feet.z + 0.1f;
        const float endZ = feet.z - 1000.0f;
        const auto links = wmoRenderer->linksAlong(glm::vec3(feet.x, feet.y, startZ), endZ);
        const auto ground = groundDown(startZ, endZ);
        for (const auto& hit : {links.collides, links.drawn}) {
            if (!hit || (ground && *ground < hit->dist)) continue;
            if (std::find(playerLinks_.begin(), playerLinks_.end(), hit->ref) == playerLinks_.end())
                playerLinks_.push_back(hit->ref);
        }
        // The first that is not a moving building's group open to the sky:
        // the one 0x007a1640, 0x007a1480 and 0x007a18d0 all stop at.
        for (const auto& ref : playerLinks_) {
            const auto g = wmoRenderer->groupArea(ref);
            if (!g || (g->dynamic && !mi::isInteriorGroup(g->flags))) continue;
            linked = g;
            playerLinkIndoors_ = mi::isInteriorGroup(g->flags);
            break;
        }
    }
    const uint32_t groundArea = terrainManager->getAreaIdAt(feet.x, feet.y).value_or(0);
    // A moving building's group names nothing (0x00782560 and 0x007a13e0
    // pass over map objects with 0x400).
    const bool linkedStill = linked && !linked->dynamic;
    if (linkedStill) {
        const auto* row = mi::areaRow(rows, linked->wmoId, linked->nameSet, linked->areaGroupId);
        playerLinkedArea_ = za::linkedArea(true, row ? row->areaId : 0, 0);
    }

    // The area the place is named by (0x0077fa00).
    std::optional<float> wmoDist;
    uint32_t wmoArea = 0;
    const float nameStartZ = feet.z + 0.33f;
    const float nameEndZ = feet.z - 1760.0f;
    if (wmoRenderer) {
        if (const auto hit = wmoRenderer->areaGroupAlong(glm::vec3(feet.x, feet.y, nameStartZ), nameEndZ)) {
            wmoDist = hit->dist;
            if (const auto g = wmoRenderer->groupArea(hit->ref)) {
                if (const auto* row = mi::areaRow(rows, g->wmoId, g->nameSet, g->areaGroupId)) wmoArea = row->areaId;
            }
        }
    }
    const uint32_t area = za::placeArea(wmoDist, wmoArea, groundDown(nameStartZ, nameEndZ), groundArea);
    if (area == 0) return;

    // AreaTable as the texts read it: ParentAreaID and the name.
    std::unordered_map<uint32_t, za::AreaInfo> areaRows;
    const auto lookup = [&](uint32_t id) -> const za::AreaInfo* {
        auto it = areaRows.find(id);
        if (it == areaRows.end()) {
            const auto parent = zoneManager->areaParent(id);
            if (!parent) return nullptr;
            it = areaRows.emplace(id, za::AreaInfo{*parent, gh->getAreaName(id)}).first;
        }
        return &it->second;
    };
    // The building's names where the player is linked to one that is not
    // moving: its group's row's (0x007a15b0), and its own row's (0x007a1500)
    // - which, nameless, gives the name of its area or, with none, of the
    // ground's. A building with no row of its own names nothing.
    std::optional<za::BuildingNames> names;
    if (linkedStill) {
        if (const auto* root = mi::areaRow(rows, linked->wmoId, linked->nameSet, -1)) {
            za::BuildingNames n;
            if (const auto* row = mi::areaRow(rows, linked->wmoId, linked->nameSet, linked->areaGroupId))
                n.group = row->name;
            n.building = root->name;
            if (n.building.empty()) n.building = gh->getAreaName(root->areaId != 0 ? root->areaId : groundArea);
            names = std::move(n);
        }
    }
    const auto next = za::texts(area, lookup, names);
    if (!next) return;
    // The event (0x005204c0). The first texts on a map are a new area, as the
    // client's first are against the zero it starts from.
    const za::Event e = zoneTextsKnown_ ? za::event(zoneTexts_, *next, playerLinkIndoors_)
                                        : za::Event::ZoneChangedNewArea;
    zoneTexts_ = *next;
    zoneTextsKnown_ = true;
    if (const char* name = za::eventName(e)) gh->fireAddonEvent(name, {});
}

float Renderer::sampleSunOcclusion(const glm::vec3& sunDir, std::string* why) const {
    const auto because = [why](std::string reason) { if (why) *why = std::move(reason); };
    if (!camera) { because("no camera"); return 1.0f; }
    const glm::vec3 eye = camera->getPosition();

    // Below the horizon there is nothing to be occluded by, and nothing to
    // flare either - the time-of-day gate in LensFlare covers the same ground.
    if (sunDir.z <= 0.0f) { because("below the horizon"); return 1.0f; }

    // Indoors the sun is behind a roof by definition, and a roof is the one
    // occluder the terrain march below cannot see. Indoors means an interior
    // group (MOGP 0x2000): a building's outdoor groups - courtyards, bridges,
    // the open plazas of Silvermoon and Sunstrider Isle - have bounding boxes
    // that take in open sky, and counting those put out the sun's glare for
    // anyone standing near one. The client asks an occlusion query of the
    // sun itself (0x009abe00), which an open courtyard does not block.
    if (wmoRenderer && wmoRenderer->isInsideInteriorWMO(eye.x, eye.y, eye.z)) {
        because("inside an interior WMO group");
        return 1.0f;
    }

    // How far to look. Far enough to clear the hill the camera is standing
    // under, and no further: past a few hundred yards a ridge on the horizon
    // is the sky's business, not the flare's.
    constexpr float kReach = 600.0f;

    if (wmoRenderer) {
        const float hit = wmoRenderer->raycastBoundingBoxes(eye, sunDir, kReach);
        if (hit < kReach) {
            because("WMO in the way at " + std::to_string(hit));
            return 1.0f;
        }
    }

    // The terrain, marched rather than intersected: the heightmap is what
    // terrain collision is here, so asking it how high the ground is under each
    // sample answers the same question an intersection would.
    //
    // Geometric steps, because the sample that decides this is almost always
    // near the eye - the lip of the slope being stood under, or the hillside
    // the camera has been pushed into - while a sample five hundred yards out
    // only has to be finer than a mountain. Nineteen of them cover the range.
    if (terrainManager) {
        for (float t = 1.0f; t < kReach; t *= 1.4f) {
            const glm::vec3 p = eye + sunDir * t;
            const std::optional<float> ground = terrainManager->getHeightAt(p.x, p.y);
            if (ground && *ground > p.z) {
                because("terrain in the way at " + std::to_string(t));
                return 1.0f;
            }
        }
    }
    because("clear");
    return 0.0f;
}

void Renderer::update(float deltaTime) {
    ZoneScopedN("Renderer::update");
    globalTime += deltaTime;
    runDeferredWorldInitStep(deltaTime);

    // Ease toward it rather than taking it. The sample is a yes or a no, and
    // walking a hill edge across the line to the sun would otherwise snap the
    // flare on and off; a quarter second of travel reads as the sun going
    // behind something.
    {
        const LightingParams* lp = lightingManager ? &lightingManager->getLightingParams() : nullptr;
        const float rate = glm::clamp(1.0f - std::exp(-deltaTime / 0.25f), 0.0f, 1.0f);
        // WOWEE_GLARE_DIAG: say every few seconds what decided the sun's
        // occlusion, the factor the glare is most often put out by.
        static const bool glareDiag = std::getenv("WOWEE_GLARE_DIAG") != nullptr;
        std::string why;
        const float sunTarget = lp ? sampleSunOcclusion(lp->sunDir, glareDiag ? &why : nullptr) : 1.0f;
        sunOcclusion_ += (sunTarget - sunOcclusion_) * rate;
        if (glareDiag && (glareDiagTimer_ -= deltaTime) <= 0.0f) {
            glareDiagTimer_ = 3.0f;
            const bool anyGroup = camera && wmoRenderer &&
                wmoRenderer->isInsideWMO(camera->getPosition().x, camera->getPosition().y,
                                         camera->getPosition().z);
            LOG_INFO("[glare-diag] sun occlusion target=", sunTarget, " eased=", sunOcclusion_,
                     " (", lp ? why : std::string("no lighting"), "); inside any WMO group=",
                     anyGroup ? "yes" : "no", " skyboxWeight=",
                     lightingManager ? LightingManager::skyboxGlareWeight(lightingManager->getSkyboxLayers())
                                     : 0.0f);
        }
        const float moonTarget = lp ? sampleSunOcclusion(lp->moonDir) : 1.0f;
        moonOcclusion_ += (moonTarget - moonOcclusion_) * rate;
    }

    auto updateStart = std::chrono::steady_clock::now();
    lastDeltaTime_ = deltaTime;

    if (wmoRenderer) wmoRenderer->resetQueryStats();
    if (m2Renderer) m2Renderer->resetQueryStats();

    if (cameraController) {
        auto cameraStart = std::chrono::steady_clock::now();
        WOWEE_PROFILE_SCOPE("camera + movement", Cpu);
        cameraController->update(deltaTime);
        auto cameraEnd = std::chrono::steady_clock::now();
        lastCameraUpdateMs = std::chrono::duration<double, std::milli>(cameraEnd - cameraStart).count();
        if (lastCameraUpdateMs > 50.0) {
            LOG_WARNING("SLOW cameraController->update: ", lastCameraUpdateMs, "ms");
        }

        // Update 3D audio listener position/orientation to match camera.
        // getUp() internally calls getRight() which calls getForward() again,
        // and we ask for getForward() once more on the same line - that's 3
        // independent trig sequences. Cache the basis vectors once.
        if (camera) {
            const glm::vec3 fwd = camera->getForward();
            const glm::vec3 worldUp(0.0f, 0.0f, 1.0f);
            glm::vec3 right = glm::cross(fwd, worldUp);
            float rLen = glm::length(right);
            right = (rLen < 1e-6f) ? glm::vec3(1.0f, 0.0f, 0.0f) : right / rLen;
            glm::vec3 up = glm::cross(right, fwd);
            float uLen = glm::length(up);
            up = (uLen < 1e-6f) ? glm::vec3(0.0f, 0.0f, 1.0f) : up / uLen;
            audio::AudioEngine::instance().setListenerPosition(camera->getPosition());
            audio::AudioEngine::instance().setListenerOrientation(fwd, up);
        }
    } else {
        lastCameraUpdateMs = 0.0;
    }

    // Visibility hardening: ensure player instance cannot stay hidden after
    // taxi/camera transitions, but preserve first-person self-hide.
    if (characterRenderer && characterInstanceId > 0 && cameraController) {
        if ((cameraController->isThirdPerson() && !cameraController->isFirstPersonView()) || (animationController_ && animationController_->isTaxiFlight())) {
            characterRenderer->setInstanceVisible(characterInstanceId, true);
        }
        // The own mount goes with the rider: in first person the client
        // draws neither (a taxi's view is always from behind).
        if (animationController_) {
            if (const uint32_t mount = animationController_->getMountInstanceId(); mount > 0) {
                const bool firstPerson = cameraController->isFirstPersonView() &&
                                         !animationController_->isTaxiFlight();
                characterRenderer->setInstanceVisible(mount, !firstPerson);
            }
        }
    }

    // Resolve WMO containment before the ambience consumes it.
    const bool canQueryWmo = (camera && wmoRenderer);
    const glm::vec3 camPos = camera ? camera->getPosition() : glm::vec3(0.0f);
    uint32_t insideWmoId = 0;
    const bool insideWmo = canQueryWmo &&
        wmoRenderer->isInsideWMO(camPos.x, camPos.y, camPos.z, &insideWmoId);
    playerIndoors_ = insideWmo;
    // Where the player is, and the zone events when that changes: on the
    // texts changing, as the client raises them (0x005204c0), rather than on
    // the camera entering a building's box - Dalaran's streets are inside a
    // dozen, and every one of them fired ZONE_CHANGED_INDOORS.
    updatePlayerArea();

    // Update lighting system
    if (lightingManager) {
        const auto* gh = core::Application::getInstance().getGameHandler();
        uint32_t mapId    = gh ? gh->getCurrentMapId() : 0;
        float gameTime    = gh ? gh->getGameTime() : -1.0f;
        // SMSG_WEATHER names a Weather.dbc row; the weather takes it from
        // there (0x007846a0). Nothing falls without one: the client makes up
        // no weather of its own, and draws it indoors as out, the ground it
        // traces against stopping it at a roof.
        if (weather && gh && gh->getWeatherSerial() != weatherSerialSeen_) {
            weatherSerialSeen_ = gh->getWeatherSerial();
            weather->setWeather(gh->getWeatherType(), gh->getWeatherIntensity(),
                                gh->isWeatherAbrupt());
        }
        // The storm light sets blend in by min(1, 4 x what the weather hands
        // the light) (0x007f3920): its eased intensity, held to 0.25, times
        // the row's Weather.dbc +0xc (0x00784850), not the server's raw
        // intensity.
        weatherLight_ = weather ? weather->lightValue() : 0.0f;
        const float weatherIntensity = weatherLight_;
        // The underwater sets are for the camera in liquid (0x007f3230 asks
        // 0x00780620 at the camera), not the player swimming.
        // How far under, and in what: the LiquidType row darkens the light
        // with depth or names a light of its own (0x007f3230).
        LightingManager::CameraLiquid cameraLiquid;
        if (waterRenderer && camera) {
            const glm::vec3 eye = camera->getPosition();
            const auto surface = waterRenderer->getNearestWaterHeightAt(eye.x, eye.y, eye.z);
            if (surface && eye.z < *surface) {
                cameraLiquid.submerged = true;
                cameraLiquid.depth = *surface - eye.z;
                if (auto lt = waterRenderer->getWaterTypeAt(eye.x, eye.y)) cameraLiquid.liquidType = *lt;
            }
        }
        // ScreenEffect's row: its light slot (0x004f7020 -> 0x007ecec0) - the
        // death set for a ghost - and the nether world's fog (0x007ed870).
        updateScreenEffect(deltaTime);
        cameraInLiquid_ = cameraLiquid.submerged;
        const bool ghost = gh && gh->isPlayerGhost();
        if (screenEffectState_.kind() == screen_effect::Kind::NetherWorld) {
            // White with the full-screen effects on (0xd45774's ffx, 1 by
            // default), as they always are here.
            lightingManager->setFogOverride(LightingManager::FogOverride{
                .end = screen_effect::kNetherFogEnd,
                .startScalar = screen_effect::kNetherFogStartScalar,
                .colour = glm::vec3(1.0f)});
        } else {
            lightingManager->setFogOverride(std::nullopt);
        }

        // The far clip for this map, and the fog's end held inside it
        // (0x00780770, 0x007f16f0).
        if (mapId != farClipMapId_) applyFarClip(mapId);
        lightingManager->setFarClip(farClip_);
        // The interior fog of the WMO group the camera is in (MFOG,
        // 0x007a1150).
        {
            std::optional<LightingManager::InteriorFog> interiorFog;
            // None for a ghost: 0x0077fb90 returns before looking when
            // PLAYER_FLAGS (the player's +0x1008 fields, +8) has 0x10,
            // PLAYER_FLAGS_GHOST.
            if (wmoRenderer && camera && !ghost) {
                if (auto f = wmoRenderer->interiorFogAt(camera->getPosition())) {
                    interiorFog = LightingManager::InteriorFog{
                        .end = f->end, .startScalar = f->startScalar, .color = f->color,
                        .liquidEnd = f->liquidEnd, .liquidStartScalar = f->liquidStartScalar,
                        .liquidColor = f->liquidColor, .flags = f->flags,
                        .distanceInside = f->distanceInside};
                }
            }
            lightingManager->setInteriorFog(interiorFog);
        }
        WOWEE_PROFILE_SCOPE("lighting", Cpu);
        lightingManager->update(characterPosition, mapId,
                                gameTime, weatherIntensity, cameraLiquid, screenEffectLightOverride_);
        // A world object - unit, player or game object - on an interior floor
        // is lit by that floor's vertex colour, not the zone's light, and on
        // a transition face partly by the zone's (0x007a0d60, 0x007c7fe0); a
        // unit's ambient eases between the two as it walks (0x007a1e90).
        if (wmoRenderer) {
            // The groups drawn in the camera's interior pass (0x007ad1f0),
            // which take the camera's fog colour, as does what stands in them.
            if (camera) {
                Frustum frustum;
                frustum.extractFromMatrix(camera->getProjectionMatrix() * camera->getViewMatrix());
                WOWEE_PROFILE_SCOPE("wmo interior pass", Cpu);
                wmoRenderer->updateInteriorPass(camera->getPosition(), &frustum);
            }
            const auto& zl = lightingManager->getLightingParams();
            // The terrain's baked shadow halves a world object's direct light
            // only with extShadowQuality below 2 (0x007a1bc0 asks 0x00873f80).
            const bool bakedShadowCounts = !shadowsEnabled || extShadowQuality_ < 2;
            // 0x007c28f0: the first WMO floor from `startZ` down a thousand
            // yards, unless the ground is nearer; with neither, up a thousand
            // yards from the feet.
            const auto objectFloor = [&](const glm::vec3& feet, float startZ, bool unit) {
                namespace wl = pipeline::wmo_doodad_light;
                const glm::vec3 start(feet.x, feet.y, startZ);
                std::optional<float> ground;
                // Not where the ground has a hole: a shaft or a cave mouth
                // cut through it is no floor, and taking the surface over
                // it as one lit the Undercity lifts as outdoors each time
                // they rose past the height of the ruins above - a flash
                // of daylight on the platform mid-ride.
                if (terrainManager && !terrainManager->isHoleAt(feet.x, feet.y))
                    ground = terrainManager->getHeightAt(feet.x, feet.y);
                auto found = wmoRenderer->floorAlong(start, startZ - wl::kFloorReach);
                if (found && wl::terrainNearer(startZ, ground, found->z)) found.reset();
                const bool groundBelow = ground && *ground <= startZ;
                if (!found && !groundBelow) found = wmoRenderer->floorAlong(feet, feet.z + wl::kFloorReach);
                wl::ObjectFloorState state;
                if (found) {
                    state.light = found->light;
                    state.onWmo = unit;
                }
                state.inBakedShadow = bakedShadowCounts && terrainManager &&
                                      terrainManager->isInBakedShadowAt(feet.x, feet.y);
                return state;
            };
            const auto inPass = [&](const glm::vec3& p) { return wmoRenderer->inInteriorPass(p); };
            if (characterRenderer) {
                characterRenderer->refreshInteriorLights(
                    [&](const glm::vec3& feet) {
                        return objectFloor(feet, feet.z + pipeline::wmo_doodad_light::kUnitFloorAbove, true);
                    },
                    inPass, zl.ambientColor, zl.diffuseColor, deltaTime);
            }
            if (m2Renderer) {
                m2Renderer->refreshGameObjectLights(
                    [&](const glm::vec3& feet, float startZ) { return objectFloor(feet, startZ, false); },
                    inPass, wmoRenderer->interiorPassGeneration(), zl.ambientColor, zl.diffuseColor, deltaTime);
            }
        }
        if (waterRenderer) {
            const auto& wl = lightingManager->getLightingParams();
            WaterRenderer::LightWaterColors wc;
            wc.oceanClose = glm::vec4(wl.oceanCloseColor, wl.oceanShallowAlpha);
            wc.oceanFar = glm::vec4(wl.oceanFarColor, wl.oceanDeepAlpha);
            wc.riverClose = glm::vec4(wl.riverCloseColor, wl.waterShallowAlpha);
            wc.riverFar = glm::vec4(wl.riverFarColor, wl.waterDeepAlpha);
            wc.sunColor = wl.sunColor;
            waterRenderer->setLightWaterColors(wc);
        }

        if (lightning) lightning->setEnabled(false);
    }

    // Sync character model position/rotation and animation with follow target
    if (characterInstanceId > 0 && characterRenderer && cameraController) {
        characterRenderer->setInstancePosition(characterInstanceId, characterPosition);

        // Movement-facing comes from camera controller and is decoupled from LMB orbit.
        bool taxiFlight = animationController_ && animationController_->isTaxiFlight();
        // The facing is the movement facing, strafing too: the server is
        // told where the character faces, and the strafe flag says which way
        // it goes from there. Where the body is drawn is the body yaw below.
        if (taxiFlight || cameraController->isMoving() || cameraController->isRightMouseHeld() ||
            cameraController->isTurningLeft() || cameraController->isTurningRight()) {
            characterYaw = cameraController->getFacingYaw();
        }
        // No turning toward the target in combat. The client turns the player
        // only as the player steers (FUN_00735f60 copies the movement facing
        // for the active mover; its face-the-target branch is for other
        // units), and this one swung the character round to its target at
        // 360 degrees a second whenever it stood still in a fight - and sent
        // that facing to the server.

        // The body is drawn at its own yaw, brought round to the facing by
        // CGUnit's animation update (FUN_0073dab0; see body_yaw.hpp): held
        // while the player turns, the spine and head looking round, and
        // shuffled after on the spot.
        const bool mounted = animationController_ && animationController_->isMounted();
        uint32_t moveFlags = 0;
        if (cameraController->isMovingForward()) moveFlags |= body_yaw::kForward;
        if (cameraController->isMovingBackward()) moveFlags |= body_yaw::kBackward;
        if (cameraController->isStrafingLeft()) moveFlags |= body_yaw::kStrafeLeft;
        if (cameraController->isStrafingRight()) moveFlags |= body_yaw::kStrafeRight;
        if (cameraController->isTurningLeft()) moveFlags |= body_yaw::kTurnLeft;
        if (cameraController->isTurningRight()) moveFlags |= body_yaw::kTurnRight;
        if (cameraController->isSwimming()) moveFlags |= body_yaw::kSwimming;
        if (cameraController->isFlightAirborne()) moveFlags |= body_yaw::kFlying;
        else if (!cameraController->isGrounded() && !cameraController->isSwimming())
            moveFlags |= body_yaw::kFalling;
        if (cameraController->isAscending()) moveFlags |= body_yaw::kAscending;
        if (cameraController->isDescending()) moveFlags |= body_yaw::kDescending;
        // Dead, the body lies as it fell (FUN_0073dab0's health test).
        bool dead = false;
        {
            uint32_t curAnim = 0;
            float curTime = 0.0f, curDuration = 0.0f;
            if (characterRenderer->getAnimationState(characterInstanceId, curAnim, curTime, curDuration))
                dead = curAnim == anim::DEATH || curAnim == anim::DEAD;
        }
        const float facingRad = body_yaw::wrap(glm::radians(characterYaw));
        if (characterBodyInstance_ != characterInstanceId) {
            characterBody_ = body_yaw::State{};
            characterBody_.body = facingRad;
            characterBodyInstance_ = characterInstanceId;
        }
        body_yaw::Input bodyIn;
        bodyIn.facing = facingRad;
        bodyIn.dt = deltaTime;
        bodyIn.moveFlags = moveFlags;
        // The camera stamps +0xabc after the units' update each frame the
        // player is not turning, so the catch-up runs for the frame's time.
        bodyIn.catchUpSeconds = deltaTime;
        bodyIn.turnRate = cameraController->getTurnRateRad();
        bodyIn.inputTurning = (moveFlags & (body_yaw::kTurnLeft | body_yaw::kTurnRight)) != 0 ||
                              cameraController->isMouseSteering();
        // Mounted, the rider sits the mount at the facing (applyMountPositioning
        // draws both there); on a taxi likewise.
        bodyIn.snap = dead || mounted || taxiFlight;
        bodyIn.hasSpine = characterRenderer->hasKeyBone(characterInstanceId, 4);
        bodyIn.hasHead = characterRenderer->hasKeyBone(characterInstanceId, 6);
        bodyIn.spineAllowed = !mounted;
        bodyIn.halveSpine = false;  // The player's own unit takes the whole lag on SpineLow first
        const body_yaw::Result body = body_yaw::update(characterBody_, bodyIn);
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(0.0f, 0.0f, characterBody_.body));
        characterRenderer->setInstanceTorsoYaw(characterInstanceId, body.spineYaw, body.headYaw);

        // Turning on the spot shuffles, standing up and alive (the stand
        // state FUN_0073dab0 asks through vfunc +0x138).
        if (animationController_) {
            animationController_->setTurnShuffle(body_yaw::turnShuffle(
                moveFlags, body.step, !dead && !mounted && !taxiFlight && !cameraController->isSitting()));
        }

        // Update animation based on movement state (delegated to AnimationController §4.2)
        if (animationController_) {
            WOWEE_PROFILE_SCOPE("player animation", Cpu);
            animationController_->updateMeleeTimers(deltaTime);
            animationController_->setDeltaTime(deltaTime);
            animationController_->updateCharacterAnimation();
        }
    }

    // Update terrain streaming
    if (terrainManager && camera) {
        auto terrStart = std::chrono::steady_clock::now();
        terrainManager->update(*camera, deltaTime);
        float terrMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - terrStart).count();
        if (terrMs > 50.0f) {
            LOG_WARNING("SLOW terrainManager->update: ", terrMs, "ms");
        }
    }

    // Update sky system (skybox time, star twinkle, clouds, celestial moon phases)
    if (skySystem) {
        WOWEE_PROFILE_SCOPE("sky", Cpu);
        skySystem->update(deltaTime);
    }
    if (updateSkyboxLayers() && skyboxModelRenderer_ && camera) {
        skyboxModelRenderer_->update(deltaTime, camera->getPosition(),
            camera->getProjectionMatrix() * camera->getViewMatrix());
    }
    { WOWEE_PROFILE_SCOPE("stars", Cpu); updateStarsModel(deltaTime); }

    // Update weather particles
    if (weather && camera) {
        Weather::FrameInput in;
        in.deltaTime = deltaTime;
        in.cameraPosition = camera->getPosition();
        in.cameraRight = camera->getRight();
        in.cameraUp = camera->getUp();
        in.playerPosition = characterPosition;
        in.playerYawDeg = characterYaw;
        in.riding = animationController_ && animationController_->isTaxiFlight();
        in.viewportWidth = activeRenderExtent_.width ? activeRenderExtent_.width
                                                     : (vkCtx ? vkCtx->getSwapchainExtent().width : 0);
        WOWEE_PROFILE_SCOPE("weather", Cpu);
        weather->update(in);
    }

    // Update lightning (storm / heavy rain)
    if (lightning && camera && lightning->isEnabled()) {
        lightning->update(deltaTime, *camera);
    }

    // Update swim effects
    if (swimEffects && camera && cameraController && waterRenderer) {
        WOWEE_PROFILE_SCOPE("swim effects", Cpu);
        swimEffects->update(*camera, *cameraController, *waterRenderer, deltaTime);
    }

    // Surface disturbance the character leaves in the water. The droplet spray
    // is thrown by SwimEffects above; this is the froth on the surface itself,
    // which has to come from the water shader to move and light with the water.
    if (waterRenderer && camera && cameraController) {
        glm::vec3 charPos = camera->getPosition();
        const glm::vec3* followTarget = cameraController->getFollowTarget();
        if (cameraController->isThirdPerson() && followTarget) {
            charPos = *followTarget;
        }

        const bool swimming = cameraController->isSwimming();
        float intensity = 0.0f;
        bool wading = false;

        if (cameraController->isMoving()) {
            if (auto waterH = waterRenderer->getWaterHeightAt(charPos.x, charPos.y)) {
                if (swimming) {
                    // A wake only exists where the swimmer meets the surface -
                    // diving deep leaves the surface undisturbed.
                    const float below = *waterH - charPos.z;
                    intensity = glm::clamp(1.0f - (below - 0.6f) / 1.4f, 0.0f, 1.0f);
                } else {
                    // Ankle-deep barely marks the water; thigh-deep throws the
                    // most, past which the character starts swimming anyway.
                    const float depth = *waterH - charPos.z;
                    if (depth > 0.03f && depth < 1.8f) {
                        wading = true;
                        intensity = glm::clamp(depth / 0.6f, 0.3f, 1.0f);
                    }
                }
            }
        }

        const float yawRad = glm::radians(cameraController->getYaw());
        const glm::vec2 travelDir(std::sin(yawRad), -std::cos(yawRad));
        WOWEE_PROFILE_SCOPE("water wake", Cpu);
        waterRenderer->updateWake(deltaTime, glm::vec2(charPos.x, charPos.y),
                                  travelDir, intensity, wading);
    }

    // Update mount dust effects
    if (mountDust) {
        mountDust->update(deltaTime);

        // Spawn dust when mounted and moving on ground
        if ((animationController_ && animationController_->isMounted()) && camera && cameraController && !(animationController_ && animationController_->isTaxiFlight())) {
            bool isMoving = cameraController->isMoving();
            bool onGround = cameraController->isGrounded();

            if (isMoving && onGround) {
                // Calculate velocity from camera direction and speed
                glm::vec3 forward = camera->getForward();
                float speed = cameraController->getMovementSpeed();
                glm::vec3 velocity = forward * speed;
                velocity.z = 0.0f;  // Ignore vertical component

                // Spawn dust at mount's feet (slightly below character position)
                float mho = animationController_ ? animationController_->getMountHeightOffset() : 0.0f;
                glm::vec3 dustPos = characterPosition - glm::vec3(0.0f, 0.0f, mho * 0.8f);
                mountDust->spawnDust(dustPos, velocity, isMoving);
            }
        }
    }
    // Update level-up effect
    if (levelUpEffect) {
        levelUpEffect->update(deltaTime);
    }
    // Update charge effect
    if (chargeEffect) {
        chargeEffect->update(deltaTime);
    }
    // Update transient spell visual instances (delegated to SpellVisualSystem §4.4)
    { WOWEE_PROFILE_SCOPE("spell visuals", Cpu); if (spellVisualSystem_) spellVisualSystem_->update(deltaTime); }


    // Launch M2 doodad animation on background thread (overlaps with character animation + audio)
    std::future<void> m2AnimFuture;
    bool m2AnimLaunched = false;
    if (m2Renderer && camera) {
        float m2DeltaTime = deltaTime;
        glm::vec3 m2CamPos = camera->getPosition();
        glm::mat4 m2ViewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
        m2AnimFuture = core::ThreadPool::frameWorkers().submit(
            [this, m2DeltaTime, m2CamPos, m2ViewProj]() {
                m2Renderer->update(m2DeltaTime, m2CamPos, m2ViewProj);
            });
        m2AnimLaunched = true;
    }

    // Update character animations (runs in parallel with M2 animation above)
    if (characterRenderer && camera) {
        WOWEE_PROFILE_SCOPE("animation: characters", Cpu);
        const glm::mat4 viewProjection = camera->getViewProjectionMatrix();
        characterRenderer->update(deltaTime, camera->getPosition(), &viewProjection);
    }

    // Update AudioEngine (cleanup finished sounds, etc.)
    audio::AudioEngine::instance().update(deltaTime);

    // M2Renderer::update may rebuild the instance spatial index when streaming
    // marked it dirty. Footprint spawning below performs an M2 floor query and
    // traverses that same index. Joining only after footsteps allowed the main
    // thread to walk unordered_map nodes while the animation worker cleared and
    // rebuilt them, producing a SIGSEGV in M2Renderer::gatherCandidates. Keep
    // the useful overlap with character animation and audio above, but finish
    // structural M2 work before any main-thread collision query.
    if (m2AnimLaunched) {
        WOWEE_PROFILE_SCOPE("join m2 animation", Cpu);
        try { m2AnimFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("M2 animation worker: ", e.what()); }
        m2AnimLaunched = false;
    }

    // Footsteps: age visual prints, then let authored footfall events add new ones.
    { WOWEE_PROFILE_SCOPE("footprints", Cpu); if (footprintRenderer) footprintRenderer->update(deltaTime); }

    // The blob shadows under the units (0x007e49e0): with shadowLOD 1 and
    // while extShadowQuality is below 1, which it is here whenever the
    // dynamic shadows are off. The ground under each is gathered now, before
    // the frame is recorded on other threads.
    if (blobShadowRenderer) {
        blobCasters_.clear();
        const int shadowLOD = std::atoi(
            addons::storedCVarValue("shadowLOD", "1").c_str());
        const int extShadowQuality = shadowsEnabled ? extShadowQuality_ : 0;
        if (characterRenderer && camera && blob_shadow::drawn(shadowLOD, extShadowQuality)) {
            characterRenderer->collectBlobShadows(blobCasters_);
        }
        blobShadowRenderer->prepare(vkCtx ? vkCtx->getCurrentFrame() : 0, blobCasters_,
                                    camera ? camera->getViewProjectionMatrix() : glm::mat4(1.0f),
                                    terrainManager.get(), wmoRenderer.get(), m2Renderer.get());
        // The circle under the target, laid on the ground the same way
        // (0x00725980 through 0x007e4370), turned with the camera's bearing.
        std::optional<blob_shadow::Projection> circle;
        if (selectionCircle_ && camera) {
            circle = selection_circle::project(selectionCircle_->position, selectionCircle_->radius,
                                               camera->getPosition());
        }
        blobShadowRenderer->prepareSelection(circle, selectionCircle_ ? selectionCircle_->color : glm::vec4(0.0f),
                                             camera ? camera->getViewProjectionMatrix() : glm::mat4(1.0f),
                                             terrainManager.get(), wmoRenderer.get(), m2Renderer.get());
        // A spell's circle on the place under the cursor (0x004f8a40).
        std::optional<blob_shadow::Projection> spellCircle;
        if (spellTargetCircle_) {
            spellCircle = spell_target_circle::project(spellTargetCircle_->position, spellTargetCircle_->radius);
        }
        blobShadowRenderer->prepareSpellTarget(spellCircle, spellTargetCircle_ && spellTargetCircle_->unacceptable,
                                               camera ? camera->getViewProjectionMatrix() : glm::mat4(1.0f),
                                               terrainManager.get(), wmoRenderer.get(), m2Renderer.get());
    }
    { WOWEE_PROFILE_SCOPE("footsteps", Cpu); if (animationController_) animationController_->updateFootsteps(deltaTime); }

    // Activity SFX + mount ambient sounds: delegated to AnimationController (§4.2)
    { WOWEE_PROFILE_SCOPE("sfx state", Cpu); if (animationController_) animationController_->updateSfxState(deltaTime); }

    // Ambient environmental sounds + zone/music transitions (delegated to AudioCoordinator)
    if (audioCoordinator_) {
        audio::ZoneAudioContext zctx;
        zctx.deltaTime = deltaTime;
        zctx.cameraPosition = camPos;
        zctx.isSwimming = cameraController ? cameraController->isSwimming() : false;
        zctx.insideWmo = insideWmo;
        zctx.insideWmoId = insideWmoId;
        if (weather) {
            auto wt = weather->getWeatherType();
            if (wt == Weather::Type::RAIN)       zctx.weatherType = 1;
            else if (wt == Weather::Type::SNOW)  zctx.weatherType = 2;
            zctx.weatherIntensity = weather->getIntensity();
        }
        if (lightingManager) {
            zctx.gameTimeHours = lightingManager->getTimeOfDayHours();
        }
        if (terrainManager) {
            auto tile = terrainManager->getCurrentTile();
            zctx.tileX = tile.x;
            zctx.tileY = tile.y;
            zctx.hasTile = true;
        }
        // Use the precise MCNK area classification when available; this avoids
        // stale server world-state zones and whole-ADT ambiguity at river banks.
        zctx.serverZoneId = getCurrentZoneId();
        zctx.zoneManager = zoneManager.get();
        zctx.screenEffectAmbienceId = screenEffectAmbience_;
        zctx.screenEffectZoneMusicId = screenEffectZoneMusic_;
        zctx.assetManager = core::Application::getInstance().getAssetManager();
        WOWEE_PROFILE_SCOPE("zone audio", Cpu);
        audioCoordinator_->updateZoneAudio(zctx);
    }

    // Update performance HUD
    if (performanceHUD) {
        performanceHUD->update(deltaTime);
    }

    // Periodic cache hygiene: drop model GPU data no longer referenced by active instances.
    static float modelCleanupTimer = 0.0f;
    modelCleanupTimer += deltaTime;
    if (modelCleanupTimer >= 5.0f) {
        // Both can wait for the device to go idle when there is something to
        // free, which shows up as a worst-case spike every five seconds.
        WOWEE_PROFILE_SCOPE("model cache cleanup (every 5s)", Cpu);
        if (wmoRenderer) {
            wmoRenderer->cleanupUnusedModels();
        }
        if (m2Renderer) {
            m2Renderer->cleanupUnusedModels();
        }
        modelCleanupTimer = 0.0f;
    }

    auto updateEnd = std::chrono::steady_clock::now();
    lastUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
}

void Renderer::runDeferredWorldInitStep(float deltaTime) {
    if (!deferredWorldInitEnabled_ || !deferredWorldInitPending_ || !cachedAssetManager) return;
    if (deferredWorldInitCooldown_ > 0.0f) {
        deferredWorldInitCooldown_ = std::max(0.0f, deferredWorldInitCooldown_ - deltaTime);
        if (deferredWorldInitCooldown_ > 0.0f) return;
    }

    switch (deferredWorldInitStage_) {
        case 0:
            if (audioCoordinator_->getAmbientSoundManager()) {
                audioCoordinator_->getAmbientSoundManager()->initialize(cachedAssetManager);
            }
            if (terrainManager && audioCoordinator_->getAmbientSoundManager()) {
                terrainManager->setAmbientSoundManager(audioCoordinator_->getAmbientSoundManager());
            }
            break;
        case 1:
            if (audioCoordinator_->getUiSoundManager()) audioCoordinator_->getUiSoundManager()->initialize(cachedAssetManager);
            break;
        case 2:
            if (audioCoordinator_->getCombatSoundManager()) audioCoordinator_->getCombatSoundManager()->initialize(cachedAssetManager);
            break;
        case 3:
            if (audioCoordinator_->getSpellSoundManager()) audioCoordinator_->getSpellSoundManager()->initialize(cachedAssetManager);
            break;
        case 4:
            if (audioCoordinator_->getMovementSoundManager()) audioCoordinator_->getMovementSoundManager()->initialize(cachedAssetManager);
            break;
        case 5:
            if (questMarkerRenderer && !questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, cachedAssetManager))
                LOG_WARNING("Quest marker renderer re-init failed (non-fatal)");
            if (skySystem) skySystem->loadTextures(cachedAssetManager);
            if (footprintRenderer && !footprintRenderer->initialize(this, vkCtx, perFrameSetLayout, cachedAssetManager))
                LOG_WARNING("Footprint renderer re-init failed (non-fatal)");
            if (blobShadowRenderer && !blobShadowRenderer->initialize(vkCtx, perFrameSetLayout, cachedAssetManager))
                LOG_WARNING("Blob shadow renderer re-init failed (non-fatal)");
            if (weather) weather->loadAssets(cachedAssetManager);
            break;
        default:
            deferredWorldInitPending_ = false;
            return;
    }

    deferredWorldInitStage_++;
    deferredWorldInitCooldown_ = 0.12f;
}

void Renderer::setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec4& color) {
    selectionCircle_ = SelectionCircle{pos, radius, color};
}

void Renderer::setSpellTargetCircle(const glm::vec3& pos, float radius, bool unacceptable) {
    spellTargetCircle_ = SpellTargetCircle{pos, radius, unacceptable};
}

void Renderer::clearSpellTargetCircle() {
    spellTargetCircle_.reset();
}

std::optional<glm::vec3> Renderer::pickGround(const glm::vec3& origin, const glm::vec3& dir,
                                              float maxDistance, uint32_t* wmoInstanceId) const {
    const glm::vec3 d = glm::normalize(dir);
    float best = maxDistance;
    bool hit = false;
    uint32_t building = 0;
    // Buildings and doodads, by their collision triangles, whatever the
    // collision focus around the player.
    if (wmoRenderer) {
        uint32_t id = 0;
        const float t = wmoRenderer->raycastBoundingBoxes(origin, d, best, true, &id);
        if (t < best) { best = t; hit = true; building = id; }
    }
    // The terrain, marched in half-yard steps and the crossing halved down.
    if (terrainManager) {
        constexpr float kStep = 0.5f;
        auto below = [&](float t) {
            const glm::vec3 p = origin + d * t;
            const auto h = terrainManager->getHeightAt(p.x, p.y);
            return h && p.z <= *h;
        };
        float prev = 0.0f;
        for (float t = kStep; t <= best; t += kStep) {
            if (below(t)) {
                float lo = prev, hi = t;
                for (int i = 0; i < 16; ++i) {
                    const float mid = 0.5f * (lo + hi);
                    (below(mid) ? hi : lo) = mid;
                }
                if (hi < best) { best = hi; hit = true; building = 0; }
                break;
            }
            prev = t;
        }
    }
    // Doodads by the upward faces of their collision meshes along the ray.
    if (m2Renderer) {
        const glm::vec3 end = origin + d * best;
        std::vector<glm::vec3> tris;
        m2Renderer->gatherBlobShadowGround(glm::min(origin, end) - glm::vec3(0.5f),
                                           glm::max(origin, end) + glm::vec3(0.5f), tris);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            // Moller-Trumbore, both faces.
            const glm::vec3 e1 = tris[i + 1] - tris[i];
            const glm::vec3 e2 = tris[i + 2] - tris[i];
            const glm::vec3 pv = glm::cross(d, e2);
            const float det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-8f) continue;
            const float inv = 1.0f / det;
            const glm::vec3 tv = origin - tris[i];
            const float u = glm::dot(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const glm::vec3 qv = glm::cross(tv, e1);
            const float v = glm::dot(d, qv) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float t = glm::dot(e2, qv) * inv;
            if (t > 0.0f && t < best) { best = t; hit = true; building = 0; }
        }
    }
    if (!hit) return std::nullopt;
    if (wmoInstanceId) *wmoInstanceId = building;
    return origin + d * best;
}

void Renderer::clearSelectionCircle() {
    selectionCircle_.reset();
}

// ========================= PostProcessPipeline delegation stubs (§4.3) =========================

PostProcessPipeline* Renderer::getPostProcessPipeline() const {
    return postProcessPipeline_.get();
}

void Renderer::setFSREnabled(bool enabled) {
    if (!postProcessPipeline_) return;
    auto req = postProcessPipeline_->setFSREnabled(enabled);
    if (req.requested) {
        pendingMsaaSamples_ = req.samples;
        msaaChangePending_ = true;
    }
}
void Renderer::setFSR2Enabled(bool enabled) {
    if (!postProcessPipeline_) return;
    // The FSR2 compute shaders need shaderStorageImageWriteWithoutFormat and
    // shaderInt16. A device without them used to be refused at startup; now it
    // starts, so the setting has to refuse instead.
    if (enabled) {
        VkContext* ctx = VkContext::globalInstance();
        if (ctx && !ctx->areFsr2ComputeFeaturesSupported()) {
            LOG_WARNING("FSR2 needs shaderStorageImageWriteWithoutFormat and shaderInt16, "
                        "which this device does not support - leaving it off");
            return;
        }
    }
    auto req = postProcessPipeline_->setFSR2Enabled(enabled, camera.get());
    if (req.requested) {
        pendingMsaaSamples_ = req.samples;
        msaaChangePending_ = true;
    }
    // A pending multisampling change loaded before the upscaler only has to
    // yield on a device that cannot resolve depth.
    if (enabled && msaaChangePending_ && pendingMsaaSamples_ > VK_SAMPLE_COUNT_1_BIT &&
        postProcessPipeline_->isFsr2BlockingMsaa()) {
        pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    }
}
void Renderer::setGrassEnabled(bool enabled) {
    if (enabled == grassEnabled_) return;
    grassEnabled_ = enabled;
    if (!enabled && grassRenderer_) {
        // Empty the population rather than just skipping the draw: a blade
        // count of zero is what stops the cull dispatching too, and the buffer
        // it was holding is the point of turning it off.
        grassRenderer_->setPopulation(nullptr, 0);
    }
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

void Renderer::setGrassScales(float density, float height) {
    // Up to triple. Density past 1 costs generation time and can meet the
    // blade cap, which thins the whole window rather than cutting a side off
    // it; height past 1 costs nothing.
    const float d = glm::clamp(density, 0.0f, 3.0f);
    const float h = glm::clamp(height, 0.5f, 3.0f);
    if (d == grassDensityScale_ && h == grassHeightScale_) return;
    grassDensityScale_ = d;
    grassHeightScale_ = h;
    // The live population was generated with the old numbers, so it has to go
    // rather than wait for the player to walk far enough to be rebuilt - and
    // so does a build in progress, which froze them when it began.
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

void Renderer::setGrassDistance(float yards) {
    // The slider's own range. The ceiling is generosity rather than promise:
    // a 0.4 yard blade is under a pixel tall past a couple of hundred yards,
    // so the far end of a long range is carried by the taller seeded stems
    // and by density, not by every blade surviving.
    const float d = glm::clamp(yards, 30.0f, 2000.0f);
    if (d == grassDistance_) return;
    grassDistance_ = d;
    if (grassRenderer_) grassRenderer_->setCullDistance(d);
    // Regenerate: the window radius and the falloff are both functions of the
    // distance, so neither the live population nor a build in progress
    // matches it any more.
    grassBuilder_ = pipeline::GrassPopulationBuilder{};
    grassWindowValid_ = false;
}

uint32_t Renderer::grassProfileFor(uint32_t effectId, uint32_t areaId) {
    // The biome table, read once. Its absence is fine - grass then looks the
    // way the effect data alone says - but a file that exists and does not
    // parse is an authoring error worth a line.
    if (!grassBiomesLoaded_) {
        grassBiomesLoaded_ = true;
        std::ifstream f("assets/grass_biomes.json");
        if (f) {
            const std::string text((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            std::string parseError;
            grassBiomes_ = pipeline::loadGrassBiomes(text, parseError);
            if (!parseError.empty()) {
                LOG_WARNING("Grass biomes: ", parseError);
            } else {
                LOG_INFO("Grass biomes: ", grassBiomes_.size(), " regions");
            }
        }
    }

    // Which biome this ground belongs to, memoised per area: the zone walk
    // costs a few map lookups and this is called for every sampled blade.
    uint32_t biomeIdx = 0;
    const auto cachedBiome = grassBiomeForArea_.find(areaId);
    if (cachedBiome != grassBiomeForArea_.end()) {
        biomeIdx = cachedBiome->second;
    } else {
        const uint32_t zoneId =
            zoneManager ? zoneManager->resolveAreaZoneId(areaId) : areaId;
        biomeIdx = grassBiomes_.findFor(areaId, zoneId);
        grassBiomeForArea_[areaId] = biomeIdx;
    }

    // Derived once per (biome, effect) and kept. The table is uploaded whole
    // whenever it grows, which happens a handful of times as the player
    // crosses into ground they have not stood on before and then stops.
    const uint64_t key = (static_cast<uint64_t>(biomeIdx) << 32) | effectId;
    const auto known = grassProfileIndex_.find(key);
    if (known != grassProfileIndex_.end()) return known->second;

    std::vector<std::string> models;
    std::vector<uint32_t> weights;
    if (terrainManager) terrainManager->getGroundEffectDoodads(effectId, models, weights);

    pipeline::GrassProfile profile = pipeline::deriveProfile(models, weights);
    if (const auto* biome = grassBiomes_.biome(biomeIdx)) {
        biome->override_.apply(profile);
    }

    uint32_t index = 0;
    if (grassProfiles_.size() < GrassRenderer::kMaxProfiles) {
        index = static_cast<uint32_t>(grassProfiles_.size());
        grassProfiles_.push_back(profile);
    }
    grassProfileIndex_[key] = index;
    return index;
}

void Renderer::updateGrassPopulation() {
    if (!grassEnabled_) return;
    if (!grassRenderer_ || !grassRenderer_->isReady() || !terrainManager) return;

    // How far out blades are generated, and how far the player may walk before
    // the window is rebuilt. Both follow the grass distance setting. The
    // margin between them is the invariant: at its stalest the window centre
    // lags the player by a full step, so the grass ahead reaches
    // (radius - step) - and that must clear the cull distance, or the field
    // ends at a visible edge on one side of the player and pops forward on
    // every rebuild. The first numbers this had put the window *inside* the
    // cull distance, which is exactly how it looked; building the radius as
    // distance + step keeps the margin by construction at every distance the
    // slider allows.
    // Constant, not scaled by the distance setting: the octave walk made
    // rebuild cost grow with the log of the window rather than its area, so
    // frequent small steps beat rare big ones - and the slack every ring
    // must carry to cover a stale centre is the step, which at three
    // hundred yards would have meant full-density generation far past the
    // near field.
    const float rebuildStep = 24.0f;
    const float windowRadius = grassDistance_ + rebuildStep;
    const float rebuildStepSq = rebuildStep * rebuildStep;

    const glm::vec3 center = characterPosition;
    if (glm::dot(center, center) <= 0.0f) return;  // no character yet

    // High over the ground - Dalaran floats some four hundred yards above
    // Crystalsong - every blade the window would hold lies far past where grass
    // is drawn, and building it cost tens of milliseconds a frame every rebuild
    // step walked through the city. Twice the window plus a margin, so a hill
    // inside the window cannot come within reach unseen.
    if (const auto ground = terrainManager->getHeightAt(center.x, center.y);
        ground && center.z - *ground > 2.0f * windowRadius + 50.0f) {
        if (grassBuilder_.active() || grassWindowValid_) {
            grassBuilder_ = pipeline::GrassPopulationBuilder{};
            grassRenderer_->setPopulation(nullptr, 0);
            grassWindowValid_ = false;
        }
        return;
    }

    // Whether a new build has to start. While one is running the comparison
    // is against its centre rather than the live window's, so walking far
    // during a long build restarts it around where the player now is instead
    // of finishing a window they have already left.
    bool needBegin;
    if (grassBuilder_.active()) {
        const glm::vec2 moved(center.x - grassBuildCenter_.x, center.y - grassBuildCenter_.y);
        needBegin = glm::dot(moved, moved) >= rebuildStepSq;
    } else if (grassWindowValid_) {
        const glm::vec2 moved(center.x - grassWindowCenter_.x, center.y - grassWindowCenter_.y);
        needBegin = glm::dot(moved, moved) >= rebuildStepSq;
    } else {
        needBegin = true;
    }
    if (!needBegin && !grassBuilder_.active()) return;

    // One chunk decoded at a time. populateArea walks cells in world order, so
    // consecutive samples land in the same chunk and this memo almost always
    // hits - without it every candidate would decode the chunk's alpha maps
    // again, which is four kilobytes a layer for each of tens of thousands.
    const pipeline::MapChunk* cached = nullptr;
    const TerrainTile* cachedTile = nullptr;
    pipeline::ChunkGrassContext context;
    auto densityFor = [this](uint32_t effectId) {
        return terrainManager->getGroundEffectDensity(effectId);
    };
    // One-shot diagnostic: which link in the chain is empty. A population of
    // zero can mean no chunk under the sample, no effect id on its layers, or
    // an effect the table grows nothing for, and the three are indistinguish-
    // able from the outside.
    static bool reportedChain = false;
    size_t noChunk = 0;
    size_t sampled = 0;

    // A chunk is 33 yards across and candidates are a third of a yard apart, so
    // a hundred consecutive samples land in the chunk the last one did. Testing
    // that chunk before searching for another is what takes this off the frame:
    // findChunkAt is a tile lookup and a 3x3 probe, and running it per sample
    // was most of the cost.
    constexpr float kUnitSize = core::coords::TILE_SIZE / 16.0f / 8.0f;

    // Chunk cells known to have no terrain under them yet. A miss is the
    // expensive answer - findChunkAt looks through every chunk of every loaded
    // tile before giving up - and right after a teleport, before the tiles
    // around have streamed in, it was the answer for every sample: seconds a
    // slice on the main thread. Cells on the map's own chunk grid, so one miss
    // covers the hundred samples that fall in the same chunk.
    constexpr float kChunkSize = kUnitSize * 8.0f;
    std::unordered_set<uint64_t> emptyCells;
    auto chunkCellKey = [](float wx, float wy) {
        const auto cx = static_cast<int32_t>(std::floor((core::coords::ZEROPOINT - wx) / kChunkSize));
        const auto cy = static_cast<int32_t>(std::floor((core::coords::ZEROPOINT - wy) / kChunkSize));
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
    };

    auto sampler = [&](float wx, float wy) -> pipeline::GrassSuitability {
        float fracX = 0.0f;
        float fracY = 0.0f;
        ++sampled;
        const pipeline::MapChunk* chunk = nullptr;
        if (cached && pipeline::TerrainMeshGenerator::chunkFractionsAt(
                          cached->position, wx, wy, kUnitSize, fracX, fracY)) {
            chunk = cached;
        } else {
            const uint64_t cell = chunkCellKey(wx, wy);
            if (emptyCells.count(cell) == 0) {
                chunk = terrainManager->findChunkAt(wx, wy, fracX, fracY, &cachedTile);
                if (!chunk) emptyCells.insert(cell);
            }
        }
        if (!chunk) { ++noChunk; return {}; }
        if (chunk != cached) {
            // The texture names live on the tile, which pipeline code never
            // sees, so they arrive as a lookup. build() uses them to keep
            // grass off made surfaces; nothing else can, because a road's
            // ground effect is as real as a meadow's.
            const TerrainTile* tile = cachedTile;
            auto textureNameFor = [tile](uint32_t texId) -> std::string {
                if (!tile || texId >= tile->terrain.textures.size()) return {};
                return tile->terrain.textures[texId];
            };
            context = pipeline::ChunkGrassContext{};
            context.build(*chunk, densityFor, textureNameFor);

            // Reported for the first few chunks that carry a made surface, at
            // a level the default log actually keeps. Grass on cobblestone has
            // survived two fixes that each looked right, so this is the code
            // saying what it sees rather than me saying what it should - and
            // aimed at the chunks in question rather than at whichever chunk
            // happened to be sampled first.
            static int reportedRoadChunks = 0;
            bool anyRoad = false;
            for (size_t i = 0; i < std::min<size_t>(chunk->layers.size(), 4); ++i) {
                anyRoad = anyRoad ||
                          pipeline::isRoadLikeTexture(textureNameFor(chunk->layers[i].textureId));
            }
            if (anyRoad && reportedRoadChunks < 4) {
                ++reportedRoadChunks;
                std::string detail;
                for (size_t i = 0; i < std::min<size_t>(chunk->layers.size(), 4); ++i) {
                    const std::string name = textureNameFor(chunk->layers[i].textureId);
                    detail += "\n    [" + std::to_string(i) + "] tex=" +
                              std::to_string(chunk->layers[i].textureId) + " '" +
                              (name.empty() ? std::string("<no name>") : name) + "' effect=" +
                              std::to_string(chunk->layers[i].effectId) +
                              (pipeline::isRoadLikeTexture(name) ? " ROAD" : "") +
                              (context.grows[i] ? " grows" : " bare");
                }
                LOG_WARNING("Grass road chunk: ", chunk->layers.size(), " layers", detail);
            }

            // Any water over this chunk. Sampled once here rather than per
            // blade: a pond's surface is flat, and three hundred thousand
            // height queries a rebuild is not.
            if (waterRenderer) {
                const float cx = chunk->position[0] - 4.0f * kUnitSize;
                const float cy = chunk->position[1] - 4.0f * kUnitSize;
                if (const auto level = waterRenderer->getWaterHeightAt(cx, cy)) {
                    context.waterHeight = *level;
                    context.hasWater = true;
                }
            }

            // The tones the ground is painted in, for colouring the blades.
            const size_t n = std::min<size_t>(chunk->layers.size(), 4);
            for (size_t i = 0; i < n; ++i) {
                const std::string name = textureNameFor(chunk->layers[i].textureId);
                if (name.empty()) continue;
                const auto tones = terrainManager->getTerrainTextureTones(name);
                context.layerShadow[i] = tones.shadow;
                context.layerHighlight[i] = tones.highlight;
                context.hasLayerColors = true;
            }
            cached = chunk;
            if (!reportedChain) {
                reportedChain = true;
                std::string layers;
                for (size_t i = 0; i < chunk->layers.size(); ++i) {
                    layers += " [" + std::to_string(i) +
                              "] effect=" + std::to_string(chunk->layers[i].effectId) +
                              " density=" +
                              std::to_string(terrainManager->getGroundEffectDensity(
                                  chunk->layers[i].effectId));
                }
                const auto fit = pipeline::evaluateGrass(context, *chunk, fracX, fracY);
                LOG_INFO("Grass chain: chunk at (", wx, ",", wy, ") frac=(", fracX, ",", fracY,
                         ") layers=", chunk->layers.size(), layers,
                         " -> suitability=", fit.suitability, " slope=", fit.slope);
            }
        }
        auto fit = pipeline::evaluateGrass(context, *chunk, fracX, fracY);
        // The terrain's verge eased toward roads; the placed world eases the
        // same number toward walls and wagon wheels.
        if (fit.suitability > 0.0f) {
            fit.wildness = std::min(fit.wildness, grassClearing_.wildness(wx, wy));
        }
        return fit;
    };

    pipeline::GrassPopulationParams params;
    params.densityScale = grassDensityScale_;
    params.baseHeight *= grassHeightScale_;
    // Full density out to the base range; past it the lattice coarsens by
    // octaves, so the long ranges the slider allows cost blades - and build
    // time - by the log of the radius rather than by its area. The slack
    // keeps blades in the buffer a rebuild step before they fade in, so a
    // ride toward them never outruns the window.
    params.fullDensityRadius = GrassRenderer::kCullDistance;
    params.ringSlack = rebuildStep;

    // Zero means the player turned grass off. Said out loud once: a density of
    // zero produces an empty population through a chain that is otherwise
    // working perfectly, and nothing about that looks like a setting rather
    // than a fault.
    if (params.densityScale <= 0.0f) {
        static bool warnedDisabled = false;
        if (!warnedDisabled) {
            warnedDisabled = true;
            LOG_WARNING("Grass: density is 0, so no grass will grow. "
                        "Raise Grass Density in Settings > Graphics to see any.");
        }
        grassBuilder_ = pipeline::GrassPopulationBuilder{};
        grassRenderer_->setPopulation(nullptr, 0);
        grassWindowCenter_ = center;
        grassWindowValid_ = true;
        return;
    }

    if (needBegin) {
        // Clearings around the built and the placed, gathered once per
        // rebuild: the terrain's own verges handle roads and paths, but a
        // building is a placement, and only its renderer knows where it is.
        std::vector<pipeline::GrassClearingSource> clearings;
        const float winMinX = center.x - windowRadius;
        const float winMaxX = center.x + windowRadius;
        const float winMinY = center.y - windowRadius;
        const float winMaxY = center.y + windowRadius;
        if (wmoRenderer) {
            wmoRenderer->collectGrassClearings(winMinX, winMinY, winMaxX, winMaxY, clearings);
        }
        if (m2Renderer) {
            m2Renderer->collectGrassClearings(winMinX, winMinY, winMaxX, winMaxY, clearings);
        }
        grassClearing_.build(std::move(clearings), winMinX, winMinY, winMaxX, winMaxY);

        grassBuilder_.begin(center.x, center.y, windowRadius, params,
                            GrassRenderer::kMaxBlades);
        grassBuildCenter_ = center;
    }

    // WOWEE_GRASS_DEBUG=1: everything about the ground the player is standing
    // on. Screenshots cannot say whether a stretch of cobble resolved to a
    // road layer that was suppressed, a dirt layer that was not, or a grass
    // layer underneath showing through - and those want three different fixes.
    // Once per build, not per slice.
    if (needBegin && rendering::envFlagEnabled("WOWEE_GRASS_DEBUG")) {
        float fx = 0.0f;
        float fy = 0.0f;
        const TerrainTile* tile = nullptr;
        if (const pipeline::MapChunk* here =
                terrainManager->findChunkAt(center.x, center.y, fx, fy, &tile)) {
            auto nameFor = [tile](uint32_t texId) -> std::string {
                if (!tile || texId >= tile->terrain.textures.size()) return {};
                return tile->terrain.textures[texId];
            };
            pipeline::ChunkGrassContext ctx;
            ctx.build(*here, densityFor, nameFor);
            std::string report;
            for (size_t i = 0; i < std::min<size_t>(here->layers.size(), 4); ++i) {
                const uint32_t texId = here->layers[i].textureId;
                const std::string name = nameFor(texId).empty() ? std::string("<none>")
                                                                : nameFor(texId);
                const bool road = pipeline::isRoadLikeTexture(name);
                report += "\n    [" + std::to_string(i) + "] effect=" +
                          std::to_string(here->layers[i].effectId) + " density=" +
                          std::to_string(terrainManager->getGroundEffectDensity(
                              here->layers[i].effectId)) +
                          (road ? " ROAD-SUPPRESSED " : " ") + name;
            }
            const auto fit = pipeline::evaluateGrass(ctx, *here, fx, fy);
            // The no-effect mask in full, plus the player's own quad. Stand on
            // a farm row or an abbey floor and this is the line that says
            // whether the data flags it and whether the bit order reads it
            // the right way round.
            const int qx = std::clamp(static_cast<int>(fx), 0, 7);
            const int qy = std::clamp(static_cast<int>(fy), 0, 7);
            LOG_INFO("Grass under player: frac=(", fx, ",", fy, ") suitability=",
                     fit.suitability, " growsAnything=", ctx.growsAnything ? 1 : 0,
                     " noEffectDoodad=0x", std::hex, here->noEffectDoodad, std::dec,
                     " thisQuad=", here->isEffectDisabled(qy, qx) ? "no-grow" : "grows",
                     " mappedLayer=", here->effectLayerFor(qy, qx),
                     report);
        }
    }

    const size_t profilesBefore = grassProfiles_.size();
    auto profileFor = [this](uint32_t effectId, uint32_t areaId) {
        pipeline::GrassProfileRef ref;
        ref.index = grassProfileFor(effectId, areaId);
        const auto& p = grassProfiles_[ref.index];
        ref.heightScale = p.heightScale;
        ref.widthScale = p.widthScale;
        ref.densityScale = p.densityScale;
        return ref;
    };

    // A bounded number of lattice blocks per frame, so a rebuild costs the
    // same per frame at any window size - a large one just takes more frames
    // and the field grows in when it lands. Since the octave walk, nearly
    // every block surveyed becomes a terrain sample, so the budget is set by
    // the cost of sampling rather than of walking: small enough to stay off
    // the frame, large enough that a default window lands in a few slices.
    //
    // Bounded by time as well: a slice of 64000 cells cost 10 to 50 ms
    // depending on how much of it was terrain, a stutter every rebuild step.
    // Small slices until the frame's share is spent; a rebuild at walking pace
    // still lands well inside the step it has before it would restart.
    constexpr size_t kCellsPerSlice = 8000;
    constexpr double kSliceBudgetMs = 6.0;
    const auto started = std::chrono::steady_clock::now();
    bool done = false;
    do {
        done = grassBuilder_.step(sampler, profileFor, kCellsPerSlice);
    } while (!done && std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started).count() < kSliceBudgetMs);

    // Only when it grew, and after every slice rather than at the end: the
    // shaders index this by blade, so it has to reach the device before the
    // population that refers to it does.
    if (grassProfiles_.size() != profilesBefore) {
        grassRenderer_->setProfiles(grassProfiles_);
    }
    const double generateMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    grassWorstGenerateMs_ = std::max(grassWorstGenerateMs_, generateMs);
    if (!done) return;

    std::vector<pipeline::GrassBladeSample>& blades = grassBuilder_.blades();
    const bool complete = grassBuilder_.complete();
    if (!complete) {
        // The window met the blade cap. The generator thins uniformly when it
        // can see this coming, so meeting it anyway means the density slider
        // and the terrain conspired past the estimate - worth a line, because
        // the visible symptom is a field that ends early on its north side.
        LOG_WARNING("Grass population truncated at ", blades.size(),
                    " blades; lower Grass Density or Grass Distance");
    }

    // WOWEE_GRASS_DUMP=1: write the first generated population to a CSV so
    // the field's actual shape around the player can be analysed offline. The
    // cull reports a stable 2-3x more grass kept looking one way than the
    // other, which is a claim about where blades are, and this answers it.
    static const bool dumpPopulation = rendering::envFlagEnabled("WOWEE_GRASS_DUMP");
    if (dumpPopulation) {
        static bool dumped = false;
        if (!dumped && !blades.empty()) {
            dumped = true;
            std::ofstream out("grass_population.csv");
            out << "# player," << center.x << "," << center.y << "," << center.z << "\n";
            for (const auto& b : blades) {
                out << b.x << "," << b.y << "," << b.z << "," << b.height << "\n";
            }
            LOG_INFO("Grass population dumped: ", blades.size(), " blades");
        }
    }

    grassRenderer_->setPopulation(blades.data(), blades.size());
    // The build's centre, not where the player stands now: they may have
    // walked most of a rebuild step while the window generated, and measuring
    // future movement from here rather than from them is what keeps the
    // stale-window margin honest.
    grassWindowCenter_ = grassBuildCenter_;
    grassWindowValid_ = true;
    ++grassRebuilds_;
    grassLastCount_ = blades.size();

    const double totalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (!blades.empty()) {
        float minZ = blades[0].z, maxZ = blades[0].z;
        float minH = blades[0].height, maxH = blades[0].height;
        for (const auto& b : blades) {
            minZ = std::min(minZ, b.z); maxZ = std::max(maxZ, b.z);
            minH = std::min(minH, b.height); maxH = std::max(maxH, b.height);
        }
        LOG_INFO("Grass extent: player=(", center.x, ",", center.y, ",", center.z,
                 ") bladeZ=[", minZ, ",", maxZ, "] height=[", minH, ",", maxH,
                 "] first=(", blades[0].x, ",", blades[0].y, ",", blades[0].z, ")");
    }
    if (blades.empty()) {
        LOG_INFO("Grass population empty: ", sampled, " samples, ", noChunk,
                 " with no chunk under them");
    }
    LOG_INFO("Grass population rebuilt: ", blades.size(), " blades, final slice ",
             static_cast<int>(totalMs), "ms (generate ", static_cast<int>(generateMs),
             "ms, density ", params.densityScale, ", distance ", grassDistance_, ")",
             complete ? "" : " - hit the blade cap");
}

void Renderer::renderWorld(game::World* world, game::GameHandler* gameHandler) {
    ZoneScopedN("Renderer::renderWorld");
    (void)world;

    // Guard against null command buffer (e.g. after VK_ERROR_DEVICE_LOST)
    if (currentCmd == VK_NULL_HANDLE) return;

    // GPU crash diagnostic: skip ALL world rendering to isolate crash source
    static const bool skipAll = (std::getenv("WOWEE_SKIP_ALL_RENDER") != nullptr);
    if (skipAll) return;

    worldDrawnLastFrame_ = true;
    worldDrawnThisFrame_ = true;

    auto renderStart = std::chrono::steady_clock::now();
    lastTerrainRenderMs = 0.0;
    lastWMORenderMs = 0.0;
    lastM2RenderMs = 0.0;


    uint32_t frameIdx = vkCtx->getCurrentFrame();
    VkDescriptorSet perFrameSet = perFrameDescSets[frameIdx];

    // GPU crash diagnostic: skip individual renderers to isolate which one faults
    static const bool envSkipWMO = (std::getenv("WOWEE_SKIP_WMO") != nullptr);
    static const bool envSkipChars = (std::getenv("WOWEE_SKIP_CHARS") != nullptr);
    static const bool envSkipM2 = (std::getenv("WOWEE_SKIP_M2") != nullptr);
    static const bool envSkipTerrain = (std::getenv("WOWEE_SKIP_TERRAIN") != nullptr);
    static const bool envSkipSky = (std::getenv("WOWEE_SKIP_SKY") != nullptr);
    // ...and the ablation switches the same passes off, one phase at a time.
    const auto ablated = [&](AblationPass p) {
        return passAblation_ && passAblation_->skip(p);
    };
    const bool skipWMO = envSkipWMO || ablated(AblationPass::WMO);
    const bool skipChars = envSkipChars || ablated(AblationPass::Characters);
    const bool skipM2 = envSkipM2 || ablated(AblationPass::M2);
    const bool skipTerrain = envSkipTerrain || ablated(AblationPass::Terrain);
    const bool skipSky = envSkipSky || ablated(AblationPass::Sky);
    const bool skipGrass = ablated(AblationPass::Grass);
    if (m2Renderer) {
        m2Renderer->setSkipGroundDetail(ablated(AblationPass::Clutter));
        // 250 yards: past the shadow distance and well past where a tree with
        // no LOD is still worth every triangle it has.
        m2Renderer->setDoodadDistanceCap(ablated(AblationPass::FarDoodads) ? 250.0f : 0.0f);
    }

    // Get time of day for sky-related rendering
    auto* skybox = skySystem ? skySystem->getSkybox() : nullptr;
    float timeOfDay = lightingManager
        ? lightingManager->getTimeOfDayHours()
        : (skybox ? skybox->getTimeOfDay() : 12.0f);
    // Two questions. The sky models are drawn whenever any is up at all, each
    // at its own weight. The procedural sky - sun, moons, stars and clouds -
    // stays under them unless one is up past 0.99 without LightSkybox flag
    // 0x2 'combine with the procedural sky', or the death model is
    // (0x007f09b0, 0x007efd00). Most Northrend models carry 0x2 and show the
    // client's sun and clouds through them; they used to be hidden at half
    // coverage whatever the flags said.
    const bool drawSkyModels = skyboxModelRenderer_ && !skyLayers_.empty();
    const bool useOriginalSkybox = drawSkyModels && lightingManager &&
        LightingManager::skyboxHidesProceduralSky(lightingManager->getSkyboxLayers());
    // The client's stars go with the procedural sky: drawn while it is, at
    // the star clock's alpha, and not under a sharp-stars point field that
    // stands in for them (0x007f09b0, 0x009abd50).
    const bool drawStarsModel = starsModelRenderer_ && starsAlpha_ > 0.0f &&
        !useOriginalSkybox && !sharpStars_;
    const auto drawStars = [&](VkCommandBuffer starCmd) {
        if (drawStarsModel && camera) starsModelRenderer_->render(starCmd, perFrameSet, *camera);
    };

    // GPU marks for the passes inside the scene pass, reserved in the order
    // they execute. Both paths use the same set so a profile taken either way
    // reads the same; on the parallel path each mark is written by whichever
    // thread records its pass, into its own secondary (see gpuReserveMarks
    // for why the primary cannot). A pass that is skipped leaves its mark
    // unwritten, and readback steps over it.
    enum SceneMark : uint32_t {
        kMarkTerrain, kMarkGrass, kMarkSky, kMarkWmo, kMarkChars,
        kMarkM2Opaque, kMarkM2Blended, kMarkM2Particles, kMarkRibbons,
        kMarkBlobShadows, kMarkWater, kMarkWorldEffects, kMarkSelection, kMarkOverlays,
        kSceneMarkCount
    };
    const uint32_t sceneMarks = vkCtx ? vkCtx->gpuReserveMarks(kSceneMarkCount)
                                      : VkContext::kNoGpuMark;
    // By value: the doodad worker carries a copy past the end of this function.
    const auto sceneMark = [this, sceneMarks](VkCommandBuffer cmd, uint32_t which, const char* label) {
        if (sceneMarks != VkContext::kNoGpuMark) vkCtx->gpuMarkAt(cmd, sceneMarks + which, label);
    };
    // The model renderer marks the end of its opaque half itself, from inside
    // the one call that draws both halves. Set every frame, none included, so
    // a mark left over from a frame whose model pass returned early is never
    // written into a frame that is not profiling.
    if (m2Renderer) {
        m2Renderer->setGpuOpaqueMark(sceneMarks != VkContext::kNoGpuMark
                                         ? sceneMarks + kMarkM2Opaque
                                         : VkContext::kNoGpuMark);
    }

    // ── Multithreaded secondary command buffer recording ──
    // Terrain, WMO, and M2 record on worker threads while main thread handles
    // sky, characters, water, and effects.  prepareRender() on main thread first
    // to handle thread-unsafe GPU allocations (descriptor pools, bone SSBOs).
    if (parallelRecordingEnabled_) {
        // --- Pre-compute state + GPU allocations on main thread (not thread-safe) ---
        auto prepStart = std::chrono::steady_clock::now();
        std::optional<core::ProfileScope> prepScope;
        if (core::FrameProfiler::enabled()) {
            prepScope.emplace("prepare (wmo, m2, sky, characters)", core::ProfileKind::Cpu);
        }
        if (wmoRenderer) wmoRenderer->prepareRender();
        auto prepWmoEnd = std::chrono::steady_clock::now();
        if (m2Renderer && camera) m2Renderer->prepareRender(frameIdx, *camera);
        if (drawSkyModels && camera)
            skyboxModelRenderer_->prepareRender(frameIdx, *camera);
        if (drawStarsModel && camera)
            starsModelRenderer_->prepareRender(frameIdx, *camera);
        auto prepM2End = std::chrono::steady_clock::now();
        if (characterRenderer) characterRenderer->prepareRender(frameIdx);
        auto prepEnd = std::chrono::steady_clock::now();
        prepScope.reset();
        const double prepWmoMs  = std::chrono::duration<double, std::milli>(prepWmoEnd - prepStart).count();
        const double prepM2Ms   = std::chrono::duration<double, std::milli>(prepM2End - prepWmoEnd).count();
        const double prepCharMs = std::chrono::duration<double, std::milli>(prepEnd - prepM2End).count();

        // --- Dispatch worker threads (terrain + WMO + M2) ---
        std::future<double> terrainFuture, wmoFuture, charFuture, postFuture;

        // Grass rides in the terrain secondary: it sits on the ground, and
        // that buffer is executed after the sky and before WMO, which is
        // exactly the order grass wants. Recording it there touches only the
        // one worker's command buffer.
        //
        // So the buffer is recorded when EITHER wants it, not when terrain
        // does. Hanging it off terrain alone meant that switching terrain off
        // took grass with it - which for a crash bisect is harmless and for an
        // ablation is the whole measurement, since the terrain phase would
        // then be reporting what terrain and grass cost together.
        const bool drawTerrain = terrainRenderer && camera && terrainEnabled && !skipTerrain;
        const bool drawGrass = grassRenderer_ && !skipGrass;
        if (drawTerrain || drawGrass) {
            terrainFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
                WOWEE_PROFILE_SCOPE("worker: terrain + grass", Worker);
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_TERRAIN);
                setSecondaryViewportScissor(cmd);
                if (drawTerrain) terrainRenderer->render(cmd, perFrameSet, *camera);
                sceneMark(cmd, kMarkTerrain, "terrain");
                if (drawGrass) grassRenderer_->render(cmd, frameIdx, perFrameSet);
                sceneMark(cmd, kMarkGrass, "grass");
                vkEndCommandBuffer(cmd);
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        if (wmoRenderer && camera && !skipWMO) {
            wmoFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
                WOWEE_PROFILE_SCOPE("worker: wmo", Worker);
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_WMO);
                setSecondaryViewportScissor(cmd);
                wmoRenderer->render(cmd, perFrameSet, *camera, &characterPosition);
                sceneMark(cmd, kMarkWmo, "wmo");
                vkEndCommandBuffer(cmd);
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        // This one outlives renderWorld - finishRenderWorld joins it, after the
        // interface - so it captures by value and touches nothing of this
        // function's but what it was handed.
        if (m2Renderer && camera && !skipM2) {
            m2Future_ = core::ThreadPool::frameWorkers().submit(
                    [this, perFrameSet, skipChars, sceneMark]() -> double {
                WOWEE_PROFILE_SCOPE("worker: m2", Worker);
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_M2);
                setSecondaryViewportScissor(cmd);
                const auto tBegin = std::chrono::steady_clock::now();
                {
                    WOWEE_PROFILE_SCOPE("m2 models (cull, sort, record)", Worker);
                    renderM2Models(cmd, perFrameSet, !skipChars);
                }
                sceneMark(cmd, kMarkM2Blended, "m2 transparent + blended characters");
                const auto tModels = std::chrono::steady_clock::now();
                {
                    WOWEE_PROFILE_SCOPE("m2 particles", Worker);
                    m2Renderer->renderM2Particles(cmd, perFrameSet);
                }
                sceneMark(cmd, kMarkM2Particles, "m2 particles");
                const auto tParts = std::chrono::steady_clock::now();
                {
                    WOWEE_PROFILE_SCOPE("ribbons", Worker);
                    m2Renderer->renderClientRibbons(cmd, perFrameSet);
                }
                sceneMark(cmd, kMarkRibbons, "ribbons");
                const auto tParticles = std::chrono::steady_clock::now();
                {
                    WOWEE_PROFILE_SCOPE("m2 end command buffer", Worker);
                    vkEndCommandBuffer(cmd);
                }

                // This worker is the critical path of renderWorld, and the
                // model pass inside it accounts for barely a fifth of what it
                // takes: the cull, the sort and the draw recording together
                // measure half a millisecond against the three this returns.
                // So the rest is timed too - the particles and ribbons that
                // share the buffer, and ending the buffer itself, which on
                // this driver is where a secondary is actually encoded.
                static const bool prof = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
                if (prof) {
                    static auto lastSaid = std::chrono::steady_clock::now();
                    const auto tEnd = std::chrono::steady_clock::now();
                    if (tEnd - lastSaid > std::chrono::seconds(10)) {
                        lastSaid = tEnd;
                        const auto ms = [](auto a, auto b) {
                            return std::chrono::duration<double, std::milli>(b - a).count();
                        };
                        LOG_WARNING("  m2 worker: begin ", ms(t0, tBegin),
                                    "ms, models ", ms(tBegin, tModels),
                                    "ms, particles ", ms(tModels, tParts),
                                    "ms, ribbons ", ms(tParts, tParticles),
                                    "ms, endCommandBuffer ", ms(tParticles, tEnd), "ms");
                    }
                }
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
            m2WorkerInFlight_.store(true, std::memory_order_release);
        }

        // --- Main thread: record sky (SEC_SKY) ---
        {
            WOWEE_PROFILE_SCOPE("record sky", Cpu);
            VkCommandBuffer cmd = beginSecondary(SEC_SKY);
            setSecondaryViewportScissor(cmd);
            if (skySystem && camera && !skipSky) {
                rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                    timeOfDay,
                    gameHandler ? gameHandler->getGameTime() : -1.0f,
                    gameHandler ? gameHandler->getWeatherIntensity() : 0.0f,
                    lightingManager ? &lightingManager->getLightingParams() : nullptr,
                    useOriginalSkybox);
                skyParams.weatherLight = weatherLight_;
                skyParams.sunOcclusion = sunOcclusion_;
                skyParams.moonOcclusion = moonOcclusion_;
                skyParams.skyboxWeight = drawSkyModels && lightingManager
                ? LightingManager::skyboxGlareWeight(lightingManager->getSkyboxLayers()) : 0.0f;
                skySystem->render(cmd, perFrameSet, *camera, skyParams, drawStars);
                if (drawSkyModels) {
                    skyboxModelRenderer_->render(cmd, perFrameSet, *camera);
                }
                // The glare steps here, on the main thread, and is drawn at
                // the end of the world by the post worker (0x007f0870).
                skySystem->updateGlare(*camera, skyParams);
            }
            sceneMark(cmd, kMarkSky, "sky");
            vkEndCommandBuffer(cmd);
        }

        // --- Main thread: record selection circle before overlay state is used by post ---
        {
            WOWEE_PROFILE_SCOPE("record selection circle", Cpu);
            VkCommandBuffer cmd = beginSecondary(SEC_SELECTION);
            setSecondaryViewportScissor(cmd);
            // On the finished world, depth tested (0x004f6f90 late in
            // CGWorldFrame::Render); queued after the world's passes below.
            if (blobShadowRenderer && camera) blobShadowRenderer->renderSelection(cmd, perFrameSet);
            if (blobShadowRenderer && camera) blobShadowRenderer->renderSpellTarget(cmd, perFrameSet);
            sceneMark(cmd, kMarkSelection, "selection circle");
            vkEndCommandBuffer(cmd);
        }

        // Character recording is independent after prepareRender() and no
        // longer shares the selection-circle overlay command buffer.
        charFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
            WOWEE_PROFILE_SCOPE("worker: characters", Worker);
            auto t0 = std::chrono::steady_clock::now();
            VkCommandBuffer cmd = beginSecondary(SEC_CHARS);
            setSecondaryViewportScissor(cmd);
            if (characterRenderer && camera && !skipChars) {
                characterRenderer->render(cmd, perFrameSet, *camera,
                                          CharacterRenderer::Phase::Opaque);
            }
            sceneMark(cmd, kMarkChars, "characters (opaque)");
            vkEndCommandBuffer(cmd);
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        });

        // Post-world systems are disjoint from terrain/WMO/M2/characters. Start
        // this after selection recording so OverlaySystem is never used from
        // two threads at once.
        postFuture = core::ThreadPool::frameWorkers().submit([&]() -> double {
            WOWEE_PROFILE_SCOPE("worker: water + effects", Worker);
            auto t0 = std::chrono::steady_clock::now();
            VkCommandBuffer cmd = beginSecondary(SEC_POST);
            setSecondaryViewportScissor(cmd);
            // On the ground the world has drawn, under the liquid (the
            // client draws them with its units, 0x00793980).
            if (blobShadowRenderer && camera) blobShadowRenderer->render(cmd, perFrameSet);
            sceneMark(cmd, kMarkBlobShadows, "blob shadows");
            if (waterRenderer && camera && !waterDrawsInContinuePass()) {
                waterRenderer->setRenderExtent(activeRenderExtent_);
                waterRenderer->render(cmd, perFrameSet, *camera, globalTime, false, frameIdx);
                sceneMark(cmd, kMarkWater, "water");
            }
            if (weather && camera) weather->render(cmd, perFrameSet);
            if (lightning && camera && lightning->isEnabled()) lightning->render(cmd, perFrameSet);
            if (swimEffects && camera && !swimEffectsDrawWithWater_) {
                swimEffects->render(cmd, perFrameSet);
            }
            if (mountDust && camera) mountDust->render(cmd, perFrameSet);
            if (chargeEffect && camera) chargeEffect->render(cmd, perFrameSet);
            if (fishingLines_ && camera) fishingLines_->render(cmd, perFrameSet);
            if (footprintRenderer && camera) footprintRenderer->render(cmd, perFrameSet, *camera);
            if (questMarkerRenderer && camera) questMarkerRenderer->render(cmd, perFrameSet, *camera);
            sceneMark(cmd, kMarkWorldEffects, "weather, dust, footprints, quest markers");

            // The sun's and the White Lady's glare, over the whole world with
            // no depth test, last before the overlays (0x007f0870 runs after
            // the world, 0x009ac400 turns the depth test off) - after the
            // water's continuation pass when there is one.
            if (skySystem && camera && !skipSky && !glareDrawsWithWater_) skySystem->renderGlare(cmd, perFrameSet);
            renderUnderwaterOverlay(cmd);
            renderPostSceneOverlays(cmd);
            sceneMark(cmd, kMarkOverlays, "glare, underwater, minimap overlay");
            vkEndCommandBuffer(cmd);
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        });

        // --- Wait for the workers, all but the doodads ---
        // Guard with try-catch: future::get() re-throws any exception from the
        // async task. Without this, a single bad_alloc in a render worker would
        // propagate as an unhandled exception and terminate the process.
        //
        // Profiled as main-thread work, not as a wait on the GPU: what shows
        // through here is the slowest worker's recording, which is CPU time.
        //
        // The doodad worker is left running through the interface, and these
        // are not, though nothing they record is needed before it either:
        // they finish in a fraction of its time, so letting them run on would
        // save nothing, and joining them here keeps what the interface can
        // race small. The post worker in particular draws the minimap, whose
        // rect and zoom the interface sets in addonWidgets.
        std::optional<core::ProfileScope> joinScope;
        if (core::FrameProfiler::enabled()) {
            joinScope.emplace("join render workers", core::ProfileKind::Cpu);
        }
        try { if (terrainFuture.valid()) lastTerrainRenderMs = terrainFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Terrain render worker: ", e.what()); }
        try { if (wmoFuture.valid()) lastWMORenderMs = wmoFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("WMO render worker: ", e.what()); }
        try { if (charFuture.valid()) (void)charFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Character render worker: ", e.what()); }
        try { if (postFuture.valid()) (void)postFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Post render worker: ", e.what()); }
        joinScope.reset();

        // The shadow pass, recorded through the interface as the doodads are.
        // Not before this point: prepareRender above is what changes the
        // casters' state on this thread - the characters' bone buffers, the
        // doodads' bone offsets - and the terrain, WMO and character workers
        // read the same renderers. Joined, they leave only the doodad worker
        // running beside it; see launchShadowWorker for what the two share.
        launchShadowWorker();

        pendingWorld_.queueTerrain = drawTerrain || drawGrass;
        pendingWorld_.queueWmo = wmoRenderer && camera && !skipWMO;
        pendingWorld_.queueM2 = m2Renderer && camera && !skipM2;
        pendingWorld_.prepWmoMs = prepWmoMs;
        pendingWorld_.prepM2Ms = prepM2Ms;
        pendingWorld_.prepCharMs = prepCharMs;

    } else {
        // ── Fallback: single-threaded inline recording (original path) ──

        if (terrainRenderer && camera && terrainEnabled && !skipTerrain) {
            auto terrainStart = std::chrono::steady_clock::now();
            terrainRenderer->render(currentCmd, perFrameSet, *camera);
            sceneMark(currentCmd, kMarkTerrain, "terrain");
            lastTerrainRenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - terrainStart).count();
        }

        // After terrain, before the world's models: grass sits on the ground and
        // is occluded by everything standing on it.
        if (grassRenderer_ && vkCtx && !skipGrass) {
            grassRenderer_->render(currentCmd, vkCtx->getCurrentFrame(), perFrameSet);
            sceneMark(currentCmd, kMarkGrass, "grass");
        }

        // Sky after the ground, for the reason given on the parallel path.
        if (skySystem && camera && !skipSky) {
            rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                timeOfDay,
                gameHandler ? gameHandler->getGameTime() : -1.0f,
                gameHandler ? gameHandler->getWeatherIntensity() : 0.0f,
                lightingManager ? &lightingManager->getLightingParams() : nullptr,
                useOriginalSkybox);
            skyParams.weatherLight = weatherLight_;
            skyParams.sunOcclusion = sunOcclusion_;
            skyParams.moonOcclusion = moonOcclusion_;
            skyParams.skyboxWeight = drawSkyModels && lightingManager
                ? LightingManager::skyboxGlareWeight(lightingManager->getSkyboxLayers()) : 0.0f;
            if (drawStarsModel) starsModelRenderer_->prepareRender(frameIdx, *camera);
            skySystem->render(currentCmd, perFrameSet, *camera, skyParams, drawStars);
            if (drawSkyModels) {
                skyboxModelRenderer_->prepareRender(frameIdx, *camera);
                skyboxModelRenderer_->render(currentCmd, perFrameSet, *camera);
            }
            skySystem->updateGlare(*camera, skyParams);
            sceneMark(currentCmd, kMarkSky, "sky");
        }

        if (wmoRenderer && camera && !skipWMO) {
            wmoRenderer->prepareRender();
            auto wmoStart = std::chrono::steady_clock::now();
            wmoRenderer->render(currentCmd, perFrameSet, *camera, &characterPosition);
            sceneMark(currentCmd, kMarkWmo, "wmo");
            lastWMORenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wmoStart).count();
        }

        if (characterRenderer && camera && !skipChars) {
            characterRenderer->prepareRender(frameIdx);
            characterRenderer->render(currentCmd, perFrameSet, *camera,
                                      CharacterRenderer::Phase::Opaque);
            sceneMark(currentCmd, kMarkChars, "characters (opaque)");
        }

        if (m2Renderer && camera && !skipM2) {
            m2Renderer->prepareRender(frameIdx, *camera);
            auto m2Start = std::chrono::steady_clock::now();
            renderM2Models(currentCmd, perFrameSet, !skipChars);
            sceneMark(currentCmd, kMarkM2Blended, "m2 transparent + blended characters");
            m2Renderer->renderM2Particles(currentCmd, perFrameSet);
            sceneMark(currentCmd, kMarkM2Particles, "m2 particles");
            m2Renderer->renderClientRibbons(currentCmd, perFrameSet);
            sceneMark(currentCmd, kMarkRibbons, "ribbons");
            lastM2RenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m2Start).count();
        }

        if (blobShadowRenderer && camera) blobShadowRenderer->render(currentCmd, perFrameSet);
        sceneMark(currentCmd, kMarkBlobShadows, "blob shadows");
        if (waterRenderer && camera && !waterDrawsInContinuePass()) {
            waterRenderer->setRenderExtent(activeRenderExtent_);
            waterRenderer->render(currentCmd, perFrameSet, *camera, globalTime, false, frameIdx);
            sceneMark(currentCmd, kMarkWater, "water");
        }
        if (weather && camera) weather->render(currentCmd, perFrameSet);
        if (lightning && camera && lightning->isEnabled()) lightning->render(currentCmd, perFrameSet);
        if (swimEffects && camera && !swimEffectsDrawWithWater_) {
            swimEffects->render(currentCmd, perFrameSet);
        }
        if (mountDust && camera) mountDust->render(currentCmd, perFrameSet);
        if (chargeEffect && camera) chargeEffect->render(currentCmd, perFrameSet);
        if (fishingLines_ && camera) fishingLines_->render(currentCmd, perFrameSet);
        if (footprintRenderer && camera) footprintRenderer->render(currentCmd, perFrameSet, *camera);
        if (questMarkerRenderer && camera) questMarkerRenderer->render(currentCmd, perFrameSet, *camera);
        sceneMark(currentCmd, kMarkWorldEffects, "weather, dust, footprints, quest markers");
        // The circle under the target, on the finished world (0x004f6f90).
        if (blobShadowRenderer && camera) blobShadowRenderer->renderSelection(currentCmd, perFrameSet);
        if (blobShadowRenderer && camera) blobShadowRenderer->renderSpellTarget(currentCmd, perFrameSet);
        sceneMark(currentCmd, kMarkSelection, "selection circle");
    }

    pendingWorld_.pending = true;
    pendingWorld_.parallel = parallelRecordingEnabled_;
    pendingWorld_.skipSky = skipSky;
    pendingWorld_.frameIdx = frameIdx;
    pendingWorld_.perFrameSet = perFrameSet;
    pendingWorld_.overlaysMark = sceneMarks != VkContext::kNoGpuMark
        ? sceneMarks + kMarkOverlays : VkContext::kNoGpuMark;
    pendingWorld_.launchMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - renderStart).count();

    // Inline recording has nothing still running to wait for, and the overlays
    // it draws read state the interface is about to change (the minimap's rect),
    // so it finishes now, exactly as it always did.
    if (!pendingWorld_.parallel) finishRenderWorld();
}

void Renderer::joinM2Worker() const {
    // Only the main thread. The others that reach the getters are the render
    // workers themselves - the post worker asks for the model renderer for its
    // fishing lines and footprints - and one of them waiting on the doodads
    // would only serialise two recordings that are meant to overlap. Nothing
    // off the main thread changes these renderers' state while a frame
    // records; anything that did would race the main thread just the same.
    if (std::this_thread::get_id() != mainThreadId_) return;
    if (!m2WorkerInFlight_.load(std::memory_order_acquire)) return;
    std::optional<core::ProfileScope> joinScope;
    if (core::FrameProfiler::enabled()) {
        joinScope.emplace("join m2 worker", core::ProfileKind::Cpu);
    }
    // Written only here, on the main thread, which is also the only reader.
    auto* self = const_cast<Renderer*>(this);
    try { if (m2Future_.valid()) self->lastM2RenderMs = m2Future_.get(); }
    catch (const std::exception& e) { LOG_ERROR("M2 render worker: ", e.what()); }
    m2WorkerInFlight_.store(false, std::memory_order_release);
}

void Renderer::finishRenderWorld() {
    syncWorldRecording();
    if (!pendingWorld_.pending) return;
    const PendingWorld pw = pendingWorld_;
    pendingWorld_.pending = false;

    // The frame may have gone since renderWorld: a lost device clears the
    // command buffer. The worker is joined above either way.
    if (currentCmd == VK_NULL_HANDLE) return;

    const auto finishStart = std::chrono::steady_clock::now();
    const uint32_t frameIdx = pw.frameIdx;
    const VkDescriptorSet perFrameSet = pw.perFrameSet;
    const bool skipSky = pw.skipSky;

    if (pw.parallel) {
        // prepareRender() does the GPU allocations that are not thread-safe, so it runs
        // on the main thread and is not covered by the worker timings. Name the culprit
        // when a frame runs long instead of leaving renderWorld as one opaque number.
        const double prepWmoMs = pw.prepWmoMs;
        const double prepM2Ms = pw.prepM2Ms;
        const double prepCharMs = pw.prepCharMs;
        const double prepTotalMs = prepWmoMs + prepM2Ms + prepCharMs;
        const double worstWorkerMs = std::max({lastTerrainRenderMs, lastWMORenderMs, lastM2RenderMs});

        // The same breakdown on a timer, not only when a frame runs long.
        //
        // renderWorld is 3.3ms of a 15.8ms frame and the CPU and the GPU are
        // within a millisecond of each other now, so what the main thread
        // spends here decides the frame as much as any pass does - and the
        // 40ms threshold below only ever speaks when something has already
        // gone wrong. This says where the steady state goes.
        static const bool frameProfile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
        if (frameProfile) {
            static auto lastSaid = std::chrono::steady_clock::now();
            const auto sayNow = std::chrono::steady_clock::now();
            if (sayNow - lastSaid > std::chrono::seconds(10)) {
                lastSaid = sayNow;
                LOG_WARNING("  renderWorld: prepare ", prepTotalMs,
                            "ms (wmo ", prepWmoMs, " m2 ", prepM2Ms,
                            " char ", prepCharMs, "), workers terrain ",
                            lastTerrainRenderMs, " wmo ", lastWMORenderMs,
                            " m2 ", lastM2RenderMs, " (worst ", worstWorkerMs,
                            "), terrain chunks drawn ",
                            terrainRenderer ? terrainRenderer->getRenderedChunkCount() : 0,
                            " culled ",
                            terrainRenderer ? terrainRenderer->getCulledChunkCount() : 0);
            }
        }

        if (prepTotalMs + worstWorkerMs > 40.0) {
            LOG_WARNING("SLOW renderWorld breakdown: prepare=", prepTotalMs,
                        "ms (wmo=", prepWmoMs, " m2=", prepM2Ms, " char=", prepCharMs,
                        ") workers: terrain=", lastTerrainRenderMs,
                        " wmo=", lastWMORenderMs, " m2=", lastM2RenderMs,
                        // Terrain is usually the critical path here, and its cost
                        // is one descriptor bind plus one draw per surviving
                        // chunk - so the counts say whether a slow frame is draw
                        // volume or something else entirely.
                        " | terrain chunks drawn=",
                        terrainRenderer ? terrainRenderer->getRenderedChunkCount() : 0,
                        " culled=",
                        terrainRenderer ? terrainRenderer->getCulledChunkCount() : 0,
                        " resident=",
                        terrainRenderer ? terrainRenderer->getChunkCount() : 0);
        }

        // --- Execute all secondary buffers in correct draw order ---
        VkCommandBuffer validCmds[8];
        const char* validLabels[8];
        uint32_t numCmds = 0;
        // Terrain first, then the sky. Every sky layer sits on the far plane
        // and depth-tests against what is already there, so drawing it after
        // the ground skips the clouds' noise on every pixel a hill covers,
        // which outdoors is most of them. Only the terrain goes ahead of it:
        // it is the one pass that is opaque throughout. Buildings carry
        // blended windows and doodads carry leaves and particles, and blended
        // pixels leave no depth behind, so a sky drawn after them would paint
        // over whichever of them stood against it.
        const auto queue = [&](VkCommandBuffer buffer, const char* label) {
            validLabels[numCmds] = label;
            validCmds[numCmds++] = buffer;
        };
        if (pw.queueTerrain)
            queue(secondaryCmds_[SEC_TERRAIN][frameIdx], "terrain");
        queue(secondaryCmds_[SEC_SKY][frameIdx], "sky");
        if (pw.queueWmo)
            queue(secondaryCmds_[SEC_WMO][frameIdx], "wmo");
        queue(secondaryCmds_[SEC_CHARS][frameIdx], "characters");
        if (pw.queueM2)
            queue(secondaryCmds_[SEC_M2][frameIdx], "m2");
        queue(secondaryCmds_[SEC_POST][frameIdx], "water/effects");
        // The client draws the target's circle once the world is down
        // (0x004f6f90 from CGWorldFrame::Render).
        queue(secondaryCmds_[SEC_SELECTION][frameIdx], "selection");

        // In one call. The marks that used to follow each of these were
        // written from the primary, which a subpass begun with
        // SECONDARY_COMMAND_BUFFERS contents does not allow - only
        // vkCmdExecuteCommands may be recorded there - so what those
        // timestamps read was undefined on every driver, not only on
        // MoltenVK, which resolves any mark to its render pass anyway. The
        // secondaries mark themselves now (sceneMark above), in the indices
        // reserved for them in this same order.
        (void)validLabels;
        if (numCmds > 0) vkCmdExecuteCommands(currentCmd, numCmds, validCmds);
    }

    // Underwater overlay and minimap - in the fallback path these run inline;
    // in the parallel path they were already recorded into SEC_POST above.
    if (!pw.parallel) {
        // The glare, over the finished world (0x007f0870, 0x009ac400).
        if (skySystem && camera && !skipSky && !glareDrawsWithWater_) skySystem->renderGlare(currentCmd, perFrameSet);
        renderUnderwaterOverlay(currentCmd);
        renderPostSceneOverlays(currentCmd);
        if (pw.overlaysMark != VkContext::kNoGpuMark)
            vkCtx->gpuMarkAt(currentCmd, pw.overlaysMark, "glare, underwater, minimap overlay");
    }

    // Water is drawn last, in a continuation of the scene pass, so that the
    // refraction copy taken just before it holds the scene WITHOUT water. Taking
    // that copy from the finished frame instead fed the water its own output:
    // a moving object left one sharp copy per frame (a train of ghosts) and the
    // brightness applied to the water compounded through the loop and pumped.
    if (waterDrawsInContinuePass() && camera) {
        vkCmdEndRenderPass(currentCmd);
        if (vkCtx) vkCtx->gpuMark(currentCmd, "scene pass end (store / resolve)");

        VkImage sceneColor = VK_NULL_HANDLE;
        VkImage sceneDepth = VK_NULL_HANDLE;
        VkExtent2D sceneExtent = vkCtx->getSwapchainExtent();
        bool depthIsMsaa = vkCtx->isDepthCopySourceMsaa();
        if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
            sceneColor = postProcessPipeline_->getSceneColorImage();
            sceneDepth = postProcessPipeline_->getSceneDepthImage();
            sceneExtent = postProcessPipeline_->getSceneRenderExtent();
            depthIsMsaa = postProcessPipeline_->sceneDepthIsMsaa();
        } else if (currentImageIndex < vkCtx->getSwapchainImages().size()) {
            sceneColor = vkCtx->getSwapchainImages()[currentImageIndex];
            sceneDepth = vkCtx->getDepthCopySourceImage();
        }

        // The opaque scene is finished and out of its pass: the ray traced
        // lighting reads its depth here, for the surfaces of the next frame.
        recordRtLighting(sceneDepth, sceneExtent, depthIsMsaa);

        if (sceneColor != VK_NULL_HANDLE && waterRenderer->isRefractionEnabled()) {
            waterRenderer->captureSceneHistory(currentCmd, sceneColor, sceneDepth,
                                               sceneExtent, depthIsMsaa,
                                               vkCtx->getCurrentFrame());
            vkCtx->gpuMark(currentCmd, "water refraction copy");
        }

        // Without MSAA the water continues into the scene's own framebuffer. With
        // MSAA it draws single-sampled into the resolved image instead, which is
        // both cheaper and what lets it leave the multisampled pass at all.
        const bool msaaOn = vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT;
        VkRenderPassBeginInfo contRp{};
        contRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        VkExtent2D waterExtent = activeRenderExtent_;
        if (msaaOn) {
            contRp.renderPass = waterRenderer->getWater1xRenderPass();
            contRp.framebuffer = waterRenderer->getWater1xFramebuffer(currentImageIndex);
            waterExtent = vkCtx->getSwapchainExtent();
        } else {
            contRp.renderPass = vkCtx->getSceneContinueRenderPass();
            contRp.framebuffer = activeFramebuffer_;
        }
        contRp.renderArea.extent = waterExtent;
        vkCmdBeginRenderPass(currentCmd, &contRp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.width = static_cast<float>(waterExtent.width);
        vp.height = static_cast<float>(waterExtent.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = waterExtent;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        waterRenderer->setRenderExtent(waterExtent);
        waterRenderer->render(currentCmd, perFrameSet, *camera, globalTime, msaaOn, frameIdx);
        if (vkCtx) vkCtx->gpuMark(currentCmd, "water");

        // Spray belongs on top of the surface it is thrown off. Recorded in the
        // scene pass it went under the water instead, which the sheet then hid -
        // barely at the shore where alpha sits near its floor, completely in the
        // deeper water you swim in.
        if (swimEffects && camera && swimEffectsDrawWithWater_) {
            swimEffects->render(currentCmd, perFrameSet);
        }

        // The glare, after the water as after the rest of the world
        // (0x004f8ea0: 0x007f0870 follows 0x0077f980's liquids).
        if (skySystem && !skipSky && glareDrawsWithWater_) skySystem->renderGlare(currentCmd, perFrameSet);

        // And the minimap, last of all: it is the interface rather than the
        // world, and nothing in the world belongs over it.
        if (minimapDrawsWithWater_) renderMinimapOverlay(currentCmd);
        if (vkCtx) vkCtx->gpuMark(currentCmd, "spray, glare, minimap over water");
    }

    lastRenderMs = pw.launchMs + std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - finishStart).count();
}

// Water can leave the scene pass only when there is a continuation pass to draw
// it in, which excludes MSAA. Everything else in the frame is unaffected.
void Renderer::syncSwimEffectsTargetPass() {
    if (!vkCtx) return;

    // Default: they stay in the scene pass, matching its MSAA sample count.
    VkRenderPass pass = vkCtx->getImGuiRenderPass();
    VkSampleCountFlagBits samples = vkCtx->getMsaaSamples();
    swimEffectsDrawWithWater_ = false;
    minimapDrawsWithWater_ = false;
    glareDrawsWithWater_ = false;

    if (waterDrawsInContinuePass()) {
        // Both continuation passes are single-sampled: with MSAA the water draws
        // into the resolved image, and without it there is nothing to resolve.
        if (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
            pass = waterRenderer->getWater1xRenderPass();
        } else {
            pass = vkCtx->getSceneContinueRenderPass();
        }
        if (pass != VK_NULL_HANDLE) {
            samples = VK_SAMPLE_COUNT_1_BIT;
            swimEffectsDrawWithWater_ = true;
            minimapDrawsWithWater_ = true;
            glareDrawsWithWater_ = true;
        } else {
            pass = vkCtx->getImGuiRenderPass();
        }
    }

    if (swimEffects) swimEffects->setTargetPass(pass, samples);
    if (skySystem && skySystem->getCelestial()) {
        skySystem->getCelestial()->setGlareTargetPass(glareDrawsWithWater_ ? pass : VK_NULL_HANDLE, samples);
    }
    // The minimap for the same reason as the spray, and it is the more visible
    // of the two: it is a fixed disc in the corner of the screen, so any water
    // on screen behind it painted straight over the terrain it draws. The
    // spray at least only lost against water it was thrown off.
    //
    // Only when water has actually left the scene pass. Where it has not - MSAA
    // onto an off-screen scene, or no continuation pass at all - water is drawn
    // before this and the minimap is already on top of it.
    if (minimap) {
        minimap->setTargetPass(minimapDrawsWithWater_ ? pass : VK_NULL_HANDLE,
                               samples);
    }
}

void Renderer::renderUnderwaterOverlay(VkCommandBuffer cmd) {
    // One implementation, called from both recording paths.
    //
    // There were two. The parallel path had this one - a waterline that
    // sweeps across the view as the eye crosses the surface - and the
    // fallback path had an older one that waited until the eye was 1.5
    // units under before tinting anything at all. Whichever path the
    // client happened to be recording with decided which of the two the
    // player got, and on the fallback path crossing the surface showed a
    // bare line with no water either side of it.
if (overlaySystem_ && waterRenderer && camera) {
        glm::vec3 camPos = camera->getPosition();
        // The default vertical reach of this query is 15 units, meant to
        // stop water on a cliff above being mistaken for water the camera
        // is in. For the underwater tint that cap is the wrong end of the
        // problem: past 15 units down the query found nothing, the
        // overlay stopped, and the scene snapped bright at a fixed depth.
        // Deep ocean is far deeper than that, so reach much further here.
        constexpr float kUnderwaterReach = 400.0f;
        auto waterH = waterRenderer->getNearestWaterHeightAt(
            camPos.x, camPos.y, camPos.z, kUnderwaterReach);
        // How far the eye is under the surface. The tint used to wait
        // until 1.5 units down and then apply to the whole screen at
        // once, so crossing the surface was a step: no tint, no tint,
        // fully tinted. Start it at the surface and let a waterline
        // sweep up the view over the crossing instead.
        // How wide the crossing really is: the half-height of the near plane in
        // world units, because that is exactly the slab of world the near plane
        // spans and therefore the only depth range over which part of it can be
        // above the surface while the rest is under. It was a flat 0.55, which
        // is a number rather than a measurement - too wide here, and wrong the
        // moment the field of view or the near plane changes.
        const float fovY = glm::radians(camera->getFovDegrees());
        const float kCrossingBand =
            std::max(0.05f, camera->getNearPlane() * std::tan(fovY * 0.5f));
        const float eyeDepth = waterH ? (*waterH - camPos.z) : -1.0f;

        // Says what it decided, so a screenshot of this can be read rather than
        // guessed at. Throttled: it is one line every few seconds, and only
        // while the eye is anywhere near the surface.
        {
            static double lastLog = 0.0;
            if (waterH && std::abs(eyeDepth) < 3.0f && (globalTime - lastLog) > 2.0) {
                lastLog = globalTime;
                LOG_INFO("underwater: camZ=", camPos.z, " waterZ=", *waterH,
                         " eyeDepth=", eyeDepth, " band=", kCrossingBand,
                         " wmoWater=", waterRenderer->isWmoWaterAt(camPos.x, camPos.y) ? 1 : 0,
                         " drawing=", (eyeDepth > 0.0f) ? 1 : 0);
            }
        }
        // From a near plane's half-height above the surface, not from the
        // surface itself.
        //
        // Above the water the near plane still cuts the surface, and the water
        // in front of that cut is not drawn at all - which is the hard band of
        // bare lake bed along the bottom of the view when standing in a lake
        // looking across it. Those pixels are looking through water and should
        // be shaded as such.
        //
        // This is only safe because the split is geometric now. It was tried
        // once against the old horizon line and had to be pulled: that test
        // could not tell a dry pixel from a wet one, so standing beside a lake
        // tinted the lower half of the view. The per-pixel test can - a pixel
        // whose ray enters the world above the surface comes out untouched -
        // so the band above the surface costs nothing where there is no water.
        if (waterH && eyeDepth > -kCrossingBand
                   && !waterRenderer->isWmoWaterAt(camPos.x, camPos.y)) {
            bool canal = false;
            if (auto lt = waterRenderer->getWaterTypeAt(camPos.x, camPos.y))
                canal = (*lt == 5 || *lt == 13 || *lt == 17);
            // Until the eye passes the surface the view is darkened by
            // looking through the water plane itself, which is strong -
            // its alpha runs up towards 0.9 with depth. Once the eye is
            // under, that plane is behind the camera and contributes
            // nothing, so this overlay is all that is left. Starting it
            // near zero made submerging brighten the scene sharply, which
            // is backwards. Begin at a strength comparable to what the
            // surface was contributing and deepen from there.
            const float depth = std::max(eyeDepth, 0.0f);
            constexpr float kSurfaceHandoff = 0.38f;  // matches the plane's own darkening
            const float depthFog = 1.0f - std::exp(-depth * (canal ? 0.25f : 0.12f));
            float fogStrength = kSurfaceHandoff + depthFog * (0.75f - kSurfaceHandoff);
            fogStrength = glm::clamp(fogStrength, kSurfaceHandoff, 0.75f);
            // In the light's fog colour - under water that is the underwater
            // set's ch7, darkened with depth by the LiquidType row (0x007f3230)
            // - rather than a fixed blue of our own.
            const glm::vec3 waterFog = lightingManager
                ? lightingManager->getLightingParams().fogColor
                : glm::vec3(0.03f, 0.09f, 0.18f);
            glm::vec4 tint(waterFog, fogStrength);

            // The seam is worked out per pixel from the surface height, so
            // there is no screen-space line to place here. Once the eye is
            // well under, the near plane is entirely below the water and there
            // is no seam left to draw - say so, and the whole view tints.
            const bool crossing = eyeDepth < kCrossingBand;
            overlaySystem_->renderWaterline(
                tint,
                glm::inverse(camera->getViewProjectionMatrix()),
                *waterH,
                // Softness and ripple in world units now: how thick the seam is
                // in yards of water, which does not change with where the
                // camera is looking.
                0.030f,   // meniscus half-thickness
                0.004f,   // ripple on the surface height
                globalTime, crossing, cmd);
        }
    }
}

void Renderer::renderPostSceneOverlays(VkCommandBuffer cmd) {
    // A ghost's world is the death light plus ffxDeath's desaturation
    // (LightingManager::update, ScreenEffects), not a tint laid over it.

    // Brightness overlay, applied before the minimap so it doesn't affect UI.
    if (overlaySystem_) {
        float br = postProcessPipeline_ ? postProcessPipeline_->getBrightness() : 1.0f;
        if (br < 0.99f) {
            // Black overlay at alpha (1-br) darkens as scene*br (a true multiply).
            overlaySystem_->renderOverlay(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f - br), cmd);
        } else if (br > 1.01f) {
            // Multiply scene by br instead of lerping to white (washout). The
            // water refraction shader divides br back out of its captured
            // scene sample so this doesn't compound through the history.
            overlaySystem_->renderBrightnessScale(br, cmd);
        }
    }

    // Unless it is following water into the pass after this one, where it is
    // drawn instead - see renderMinimapOverlay's caller below the water.
    if (!minimapDrawsWithWater_) renderMinimapOverlay(cmd);
}

/// The minimap disc, over whatever has been drawn so far.
void Renderer::renderMinimapOverlay(VkCommandBuffer cmd) {
    if (minimap && minimap->isEnabled() && camera && window) {
        glm::vec3 minimapCenter = camera->getPosition();
        if (cameraController && cameraController->isThirdPerson())
            minimapCenter = characterPosition;
        // The player's arrow is the interface's (MinimapArrow, drawn with the
        // blips), not part of the map.
        minimap->render(cmd, *camera, minimapCenter,
                        window->getWidth(), window->getHeight());
    }
}

bool Renderer::waterDrawsInContinuePass() const {
    if (!waterRenderer || !vkCtx) return false;
    if (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
        // The 1x water pass targets the swapchain directly, so it cannot serve a
        // frame whose scene went to an off-screen post-processing target.
        const bool offscreenScene =
            postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE;
        return !offscreenScene && waterRenderer->hasWater1xPass();
    }
    return vkCtx->getSceneContinueRenderPass() != VK_NULL_HANDLE;
}

// initPostProcess(), resizePostProcess(), shutdownPostProcess() removed -
// post-process pipeline is now handled by Vulkan (Phase 6 cleanup).

void Renderer::setActiveMapName(const std::string& name) {
    if (terrainManager) terrainManager->setMapName(name);
    if (minimap) minimap->setMapName(name);
    if (worldMap) worldMap->setMapName(name);
    if (wmoRenderer) wmoRenderer->setMapName(name);
}

bool Renderer::initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName) {
    syncWorldRecording();
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Initializing renderers for map: ", mapName);

    // Scan for custom zones on first initialization
    if (customZones_.empty()) {
        customZones_ = pipeline::CustomZoneDiscovery::scan({"custom_zones", "output"});
        if (!customZones_.empty()) {
            LOG_INFO("=== Custom Zones Available ===");
            for (const auto& z : customZones_) {
                LOG_INFO("  ", z.name, " (", z.directory, ")",
                         z.hasCreatures ? " [NPCs]" : "",
                         z.hasQuests ? " [Quests]" : "");
            }
            LOG_INFO("==============================");
        }
    }

    // Create terrain renderer if not already created
    if (!terrainRenderer) {
        terrainRenderer = std::make_unique<TerrainRenderer>();
        if (!terrainRenderer->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_ERROR("Failed to initialize terrain renderer");
            terrainRenderer.reset();
            return false;
        }
        terrainRenderer->setRtScene(rtScene_.get());
        // 0x0078de60: "Specular enabled on restart." - latched here, once.
        clientSpecular_ = pendingClientSpecular_;
        terrainRenderer->setSpecular(clientSpecular_);
        if (shadowRenderPass != VK_NULL_HANDLE) {
            terrainRenderer->initializeShadow(shadowRenderPass);
        }
    } else if (!terrainRenderer->hasShadowPipeline() && shadowRenderPass != VK_NULL_HANDLE) {
        terrainRenderer->initializeShadow(shadowRenderPass);
    }

    // Create water renderer if not already created
    if (!waterRenderer) {
        waterRenderer = std::make_unique<WaterRenderer>();
        if (!waterRenderer->initialize(vkCtx, perFrameSetLayout)) {
            LOG_ERROR("Failed to initialize water renderer");
            waterRenderer.reset();
        } else {
            waterRenderer->loadClientLiquids(assetManager);
            waterRenderer->setEnhancedWater(enhancedWater_);
            waterRenderer->setRefractionEnabled(enhancedWater_);
            waterRenderer->setClientSpecular(clientSpecular_);
        }
    }

    // Create minimap if not already created
    if (!minimap) {
        minimap = std::make_unique<Minimap>();
        if (!minimap->initialize(vkCtx, perFrameSetLayout)) {
            LOG_ERROR("Failed to initialize minimap");
            minimap.reset();
        }
    }

    // Create world map if not already created
    if (!worldMap) {
        worldMap = std::make_unique<WorldMap>();
        if (!worldMap->initialize(vkCtx, assetManager)) {
            LOG_ERROR("Failed to initialize world map");
            worldMap.reset();
        }
    }

    // Create M2, WMO, and Character renderers
    if (!m2Renderer) {
        m2Renderer = std::make_unique<M2Renderer>();
        if (!m2Renderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("M2Renderer initialization failed");
        m2Renderer->setRtScene(rtScene_.get());
        if (swimEffects) {
            swimEffects->setM2Renderer(m2Renderer.get());
        }
        // Initialize SpellVisualSystem once M2Renderer is available (§4.4)
        if (!spellVisualSystem_) {
            spellVisualSystem_ = std::make_unique<SpellVisualSystem>();
            spellVisualSystem_->initialize(m2Renderer.get(), this);
        }
    }

    // The original client's skies are camera-centered M2 models selected
    // through LightParams and LightSkybox, on every map that names one - not
    // Outland alone, which is where this was built and where it stayed. The
    // lookup was opened to all maps and this was not, so it went on doing
    // nothing anywhere else: without the renderer there is nothing to draw
    // into.
    //
    // It keeps its own no-depth renderer so the sky draws behind terrain and
    // never enters world collision.
    if (!skyboxModelRenderer_) {
        skyboxModelRenderer_ = std::make_unique<M2Renderer>();
        skyboxModelRenderer_->setSkyMode(true);
        if (!skyboxModelRenderer_->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_WARNING("Sky M2 renderer initialization failed");
            skyboxModelRenderer_.reset();
        }
    }
    if (!starsModelRenderer_) {
        starsModelRenderer_ = std::make_unique<M2Renderer>();
        starsModelRenderer_->setSkyMode(true);
        if (!starsModelRenderer_->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_WARNING("Stars M2 renderer initialization failed");
            starsModelRenderer_.reset();
        }
    }

    // HiZ occlusion culling disabled - the pyramid build + blocking fence was
    // the main frame-rate bottleneck.  GPU frustum culling alone provides good
    // draw-call reduction without the per-frame GPU stall.  HiZ can be re-
    // enabled once the pyramid build is moved to an async compute queue.
    if (!wmoRenderer) {
        wmoRenderer = std::make_unique<WMORenderer>();
        if (!wmoRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("WMORenderer initialization failed");
        wmoRenderer->setRtScene(rtScene_.get());
        if (shadowRenderPass != VK_NULL_HANDLE) {
            if (!wmoRenderer->initializeShadow(shadowRenderPass))
                LOG_WARNING("WMO shadow pipeline initialization failed");
        }
    }

    // Renderer components can be recreated during map transitions. Restore the
    // configured view distance instead of falling back to their defaults.
    setViewDistance(viewDistance_);
    setSharpStars(sharpStars_);

    // Initialize shadow pipelines for M2 if not yet done
    if (m2Renderer && shadowRenderPass != VK_NULL_HANDLE && !m2Renderer->hasShadowPipeline()) {
        if (!m2Renderer->initializeShadow(shadowRenderPass))
            LOG_WARNING("M2 shadow pipeline initialization failed");
    }
    if (!characterRenderer) {
        characterRenderer = std::make_unique<CharacterRenderer>();
        if (!characterRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
            LOG_ERROR("CharacterRenderer initialization failed");
        if (shadowRenderPass != VK_NULL_HANDLE) {
            if (!characterRenderer->initializeShadow(shadowRenderPass))
                LOG_WARNING("Character shadow pipeline initialization failed");
        }
    }

    // Initialize AnimationController (§4.2)
    if (!animationController_) {
        animationController_ = std::make_unique<AnimationController>();
        animationController_->initialize(this);
    }

    // Create and initialize terrain manager
    if (!terrainManager) {
        terrainManager = std::make_unique<TerrainManager>();
        if (!terrainManager->initialize(assetManager, terrainRenderer.get())) {
            LOG_ERROR("Failed to initialize terrain manager");
            terrainManager.reset();
            return false;
        }
        // Set water renderer for terrain streaming
        if (waterRenderer) {
            terrainManager->setWaterRenderer(waterRenderer.get());
        }
        // Set M2 renderer for doodad loading during streaming
        if (m2Renderer) {
            terrainManager->setM2Renderer(m2Renderer.get());
        }
        // Set WMO renderer for building loading during streaming
        if (wmoRenderer) {
            terrainManager->setWMORenderer(wmoRenderer.get());
        }
        // A WMO's child M2 doodads - a ship's sails, its paddlewheel - are moved
        // and destroyed through this pointer. It was never set, so every one of
        // those paths was behind a null check that never passed: the doodads
        // were created at the origin, never given their parent's transform, and
        // so drawn at the middle of the map rather than on the ship. Static
        // world doodads were unaffected, because terrain streaming places those
        // at their world position itself and never goes through the parent.
        if (wmoRenderer && m2Renderer) {
            wmoRenderer->setM2Renderer(m2Renderer.get());
        }
        // Set ambient sound manager for environmental audio emitters
        if (audioCoordinator_->getAmbientSoundManager()) {
            terrainManager->setAmbientSoundManager(audioCoordinator_->getAmbientSoundManager());
        }
        // Pass asset manager to character renderer for texture loading
        if (characterRenderer) {
            characterRenderer->setAssetManager(assetManager);
        }
        // Wire asset manager to minimap for tile texture loading
        if (minimap) {
            minimap->setAssetManager(assetManager);
        }
        // Wire terrain manager, WMO renderer, and water renderer to camera controller
        if (cameraController) {
            cameraController->setTerrainManager(terrainManager.get());
            if (wmoRenderer) {
                cameraController->setWMORenderer(wmoRenderer.get());
            }
            if (m2Renderer) {
                cameraController->setM2Renderer(m2Renderer.get());
            }
            if (waterRenderer) {
                cameraController->setWaterRenderer(waterRenderer.get());
            }
        }
    }

    // Set map name on sub-renderers
    setActiveMapName(mapName);

    // Initialize audio managers
    if (audioCoordinator_->getMusicManager() && assetManager && !cachedAssetManager) {
        audio::AudioEngine::instance().setAssetManager(assetManager);
        audioCoordinator_->getMusicManager()->initialize(assetManager);
        if (audioCoordinator_->getFootstepManager()) {
            audioCoordinator_->getFootstepManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getActivitySoundManager()) {
            audioCoordinator_->getActivitySoundManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getMountSoundManager()) {
            audioCoordinator_->getMountSoundManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getNpcVoiceManager()) {
            audioCoordinator_->getNpcVoiceManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getPlayerVoiceManager()) {
            audioCoordinator_->getPlayerVoiceManager()->initialize(assetManager);
        }
        if (!deferredWorldInitEnabled_) {
            if (audioCoordinator_->getAmbientSoundManager()) {
                audioCoordinator_->getAmbientSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getUiSoundManager()) {
                audioCoordinator_->getUiSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getCombatSoundManager()) {
                audioCoordinator_->getCombatSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getSpellSoundManager()) {
                audioCoordinator_->getSpellSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getMovementSoundManager()) {
                audioCoordinator_->getMovementSoundManager()->initialize(assetManager);
            }
            if (questMarkerRenderer) {
                if (!questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
                    LOG_WARNING("Quest marker renderer initialization failed (non-fatal)");
            }
            if (skySystem) skySystem->loadTextures(assetManager);
            if (footprintRenderer) {
                if (!footprintRenderer->initialize(this, vkCtx, perFrameSetLayout, assetManager))
                    LOG_WARNING("Footprint renderer initialization failed (non-fatal)");
            }
            if (blobShadowRenderer) {
                if (!blobShadowRenderer->initialize(vkCtx, perFrameSetLayout, assetManager))
                    LOG_WARNING("Blob shadow renderer initialization failed (non-fatal)");
            }

            if (core::envFlagEnabled("WOWEE_PREWARM_ZONE_MUSIC", false)) {
                if (zoneManager) {
                    for (const auto& musicPath : zoneManager->getAllMusicPaths()) {
                        audioCoordinator_->getMusicManager()->preloadMusic(musicPath);
                    }
                }
                const std::string tavernRemix =
                    game::ZoneManager::resolveOriginalMusicFile("TavernAllianceREMIX.mp3");
                const std::vector<std::string> tavernTracks = {
                    tavernRemix.empty()
                        ? std::string("Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance01.mp3")
                        : tavernRemix,
                    "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance02.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern1A.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern2A.mp3",
                };
                for (const auto& musicPath : tavernTracks) {
                    audioCoordinator_->getMusicManager()->preloadMusic(musicPath);
                }
            }
        } else {
            deferredWorldInitPending_ = true;
            deferredWorldInitStage_ = 0;
            deferredWorldInitCooldown_ = 0.25f;
        }

        cachedAssetManager = assetManager;

        // Enrich zone music from DBC if not already done (e.g. asset manager was null at init).
        if (zoneManager && assetManager) {
            zoneManager->enrichFromDBC(assetManager);
        }
    }

    // Snap camera to ground
    if (cameraController) {
        cameraController->reset();
    }

    return true;
}

bool Renderer::loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath) {
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Loading test terrain: ", adtPath);

    // Extract map name from ADT path for renderer initialization
    std::string mapName;
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            mapName = filename.substr(0, firstUnderscore != std::string::npos ? firstUnderscore : filename.size());
        }
    }

    // Initialize all sub-renderers
    if (!initializeRenderers(assetManager, mapName)) {
        return false;
    }

    // Parse tile coordinates from ADT path
    // Format: World\Maps\{MapName}\{MapName}_{X}_{Y}.adt
    int tileX = 32, tileY = 49;  // defaults
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            if (firstUnderscore != std::string::npos) {
                size_t secondUnderscore = filename.find('_', firstUnderscore + 1);
                if (secondUnderscore != std::string::npos) {
                    size_t dot = filename.find('.', secondUnderscore);
                    if (dot != std::string::npos) {
                        try {
                            tileX = std::stoi(filename.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1));
                            tileY = std::stoi(filename.substr(secondUnderscore + 1, dot - secondUnderscore - 1));
                        } catch (...) {
                            LOG_WARNING("Failed to parse tile coords from: ", filename);
                        }
                    }
                }
            }
        }
    }

    LOG_INFO("Enqueuing initial tile [", tileX, ",", tileY, "] via terrain manager");

    // Enqueue the initial tile for async loading (avoids long sync stalls)
    if (!terrainManager->enqueueTile(tileX, tileY)) {
        LOG_ERROR("Failed to enqueue initial tile [", tileX, ",", tileY, "]");
        return false;
    }

    terrainLoaded = true;

    LOG_INFO("Test terrain loaded successfully!");
    LOG_INFO("  Chunks: ", terrainRenderer->getChunkCount());
    LOG_INFO("  Triangles: ", terrainRenderer->getTriangleCount());

    return true;
}

void Renderer::setWireframeMode(bool enabled) {
    if (terrainRenderer) {
        terrainRenderer->setWireframe(enabled);
    }
}

// One line naming how far each of the three actually drew this frame.
//
// "Distant objects float with no terrain" is a disagreement between two
// distances, and every attempt to settle it by reading the code picked the
// wrong one of the three places that compute it. This prints the answer:
// if terrain stops short of the doodads the fault is in loading the tiles,
// and if the doodads run past the setting the fault is in the cull. It is at
// warning because the default log is warnings only, and it prints on a change
// of half a tile rather than every frame.
void Renderer::logViewDistanceDiag() {
    static const bool enabled = std::getenv("WOWEE_VIEW_DIAG") != nullptr;
    if (!enabled) return;

    const float terrainFurthest = terrainRenderer
        ? terrainRenderer->getFurthestDrawnDistance() : 0.0f;
    const float m2Furthest = m2Renderer
        ? m2Renderer->getFurthestDrawnDistance() : 0.0f;

    if (std::abs(terrainFurthest - diagTerrainFurthest_) < 266.0f &&
        std::abs(m2Furthest - diagM2Furthest_) < 266.0f) {
        return;
    }
    diagTerrainFurthest_ = terrainFurthest;
    diagM2Furthest_ = m2Furthest;

    LOG_WARNING("view distance ", static_cast<int>(viewDistance_),
                ", far clip ", static_cast<int>(farClip_),
                ": terrain drew to ", static_cast<int>(terrainFurthest),
                ", doodads to ", static_cast<int>(m2Furthest),
                ", tiles loaded to ", getTerrainLoadRadius(),
                " (", static_cast<int>(getTerrainLoadRadius() *
                                       core::coords::TILE_SIZE), ")");
}

void Renderer::setSharpStars(bool enabled) {
    syncWorldRecording();
    sharpStars_ = enabled;
    // Two halves of one switch: the sky model stops drawing its star layer and
    // the client's own point stars take its place. Setting either alone gives a
    // sky with no stars or a sky with two sets of them.
    if (m2Renderer) m2Renderer->setSuppressBakedStars(sharpStars_);
    if (skySystem) skySystem->setProceduralStarsEnabled(sharpStars_);
}

void Renderer::setViewDistance(float distance) {
    syncWorldRecording();
    viewDistance_ = glm::clamp(distance, 400.0f, 2400.0f);
    applyFarClip(farClipMapId_);
}

// The setting is the client's farclip cvar; what the world is drawn to is
// that held to the map's limit, 791 yards before map 530 and 1583 from it
// (0x00780770, on a change of setting 0x00780800 and on a new map
// 0x00781430). The fog ends inside it too (LightingManager::setFarClip).
void Renderer::applyFarClip(uint32_t mapId) {
    farClipMapId_ = mapId;
    const bool moreThan1GB = core::MemoryMonitor::getInstance().getTotalRAM() == 0 ||
                             core::MemoryMonitor::getInstance().getTotalRAM() > 0x40000000ull;
    farClip_ = daynight::clientFarClip(viewDistance_, mapId, false, moreThan1GB);

    if (terrainRenderer) terrainRenderer->setViewDistance(farClip_);
    if (wmoRenderer) wmoRenderer->setViewDistance(farClip_);
    if (m2Renderer) m2Renderer->setViewDistance(farClip_);
    if (terrainManager) {
        terrainManager->setLoadRadius(getTerrainLoadRadius());
        terrainManager->setUnloadRadius(getTerrainUnloadRadius());
    }
}

int Renderer::getTerrainLoadRadius() const {
    constexpr float kAdtTileSize = core::coords::TILE_SIZE;
    return glm::clamp(static_cast<int>(std::ceil(farClip_ / kAdtTileSize)) + 1, 2, 6);
}
void Renderer::renderHUD() {
    if (currentCmd == VK_NULL_HANDLE) return;
    if (performanceHUD && camera) {
        performanceHUD->render(this, camera.get());
    }
    // Its own window, and not only with the HUD above: the HUD is a debug
    // build's F1, and the profiler has to work in the build that is slow.
    if (performanceHUD && core::FrameProfiler::enabled()) {
        performanceHUD->renderFrameProfile(this);
    }
}

// ──────────────────────────────────────────────────────
// Shadow mapping helpers
// ──────────────────────────────────────────────────────

// initShadowMap() and compileShadowShader() removed - shadow resources now created
// in createPerFrameResources() as part of the Vulkan shadow infrastructure.

namespace {

/// How far out cascade `i` of `count` reaches, in yards, for a shadow
/// distance of `distance`: the practical split (Zhang et al.), a mix of the
/// logarithmic split from a yard out and the even one, three parts to one.
/// The last is the distance itself. Two cascades at the default 300 yards
/// split at about 50; three at about 30 and 85.
float shadowCascadeSplit(int i, int count, float distance) {
    if (i + 1 >= count) return distance;
    constexpr float kLambda = 0.75f;
    constexpr float kFirst = 1.0f;
    const float f = static_cast<float>(i + 1) / static_cast<float>(count);
    const float logSplit = kFirst * std::pow(distance / kFirst, f);
    const float evenSplit = kFirst + (distance - kFirst) * f;
    return kLambda * logSplit + (1.0f - kLambda) * evenSplit;
}

}  // namespace

// The cascades: shadowCascadeCount_ squares, all centred on the player, all
// looking down the same light direction over the same depth range - the
// light 3 x the shadow distance back from the centre and 6.5 x it deep - so a
// point has the same depth in every one of them. Only the half-extent
// differs, from the split above. Each is snapped to its own texel grid and
// drawn into its own tile of the atlas. Returns the outer cascade's matrix,
// which is the single map's as it always was, or zero before the player has
// a position.
glm::mat4 Renderer::computeLightSpaceMatrix() {
    activeShadowCascades_ = 0;
    const float kShadowLightDistance = shadowDistance_ * 3.0f;
    const float kShadowFarPlane = shadowFarPlane(shadowDistance_);

    // The way the light travels: from the sun as it is drawn on the sky.
    glm::vec3 sunDir = sunTravelDirection();
    // Shadow camera expects light rays pointing downward in render space (Z up).
    // Some profiles/opcode paths provide the opposite convention; normalize here.
    if (sunDir.z > 0.0f) {
        sunDir = -sunDir;
    }
    // Keep a minimum downward component so the frustum doesn't collapse at grazing angles.
    if (sunDir.z > -0.15f) {
        sunDir.z = -0.15f;
        sunDir = glm::normalize(sunDir);
    }

    // LightingManager already smooths the directional light every frame. Keep
    // that continuous direction for the shadow projection as well. Quantizing
    // it into 0.5-degree steps made the entire 600-yard shadow footprint rotate
    // in a single frame, producing a wide light-switch flicker whenever the
    // threshold was crossed. Translation remains stabilized by texel snapping.

    // Shadow center follows the player directly; texel snapping below prevents
    // camera translation from shimmering the projection.
    glm::vec3 desiredCenter = characterPosition;
    if (!shadowCenterInitialized) {
        if (glm::dot(desiredCenter, desiredCenter) < 1.0f) {
            return glm::mat4(0.0f);
        }
        shadowCenterInitialized = true;
    }
    // Stable light-space axes (independent of center position)
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if (std::abs(glm::dot(sunDir, up)) > 0.99f) {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    glm::vec3 lightRight = glm::normalize(glm::cross(sunDir, up));
    glm::vec3 lightUp = glm::normalize(glm::cross(lightRight, sunDir));

    const int cascades = std::clamp(shadowCascadeCount_, 1, kMaxShadowCascades);
    const uint32_t tileSide = SHADOW_MAP_SIZE;
    const float atlasW = static_cast<float>(shadowAtlasWidth(cascades, tileSide));
    const float atlasH = static_cast<float>(shadowAtlasHeight(cascades, tileSide));
    for (int c = 0; c < cascades; ++c) {
        ShadowCascade& sc = shadowCascades_[c];
        const float halfExtent = shadowCascadeSplit(c, cascades, shadowDistance_);

        // Snap the centre to this cascade's texel grid so its projection is
        // perfectly stable while moving: along the light's right and up axes
        // (constant for the frame whatever the centre), and not along its
        // depth, which every cascade shares. This eliminates the sub-texel
        // shifts that cause shadow shimmer.
        const float texelWorld = (2.0f * halfExtent) / static_cast<float>(tileSide);
        glm::vec3 center = desiredCenter;
        float dotR = glm::dot(center, lightRight);
        float dotU = glm::dot(center, lightUp);
        dotR = std::floor(dotR / texelWorld) * texelWorld;
        dotU = std::floor(dotU / texelWorld) * texelWorld;
        float dotD = glm::dot(center, sunDir);  // depth axis unchanged
        center = lightRight * dotR + lightUp * dotU + sunDir * dotD;

        glm::mat4 lightView = glm::lookAt(center - sunDir * kShadowLightDistance, center, up);
        glm::mat4 lightProj = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent,
                                         kShadowNearPlane, kShadowFarPlane);
        lightProj[1][1] *= -1.0f; // Vulkan Y-flip for shadow pass

        // Its tile: left to right, in one row or two (shadowAtlasInRow).
        const bool inRow = shadowAtlasInRow(cascades, tileSide);
        const uint32_t tileX = static_cast<uint32_t>(inRow ? c : c % 2) * tileSide;
        const uint32_t tileY = static_cast<uint32_t>(inRow ? 0 : c / 2) * tileSide;

        // Clip space to the tile's atlas UV, the way the viewport puts it
        // there: x from -1..1 to tileX..tileX+side over the atlas width, and
        // y the same (the projection's flip already matches the viewport's
        // downward y). Depth is left alone.
        glm::mat4 toTile(1.0f);
        toTile[0][0] = 0.5f * static_cast<float>(tileSide) / atlasW;
        toTile[1][1] = 0.5f * static_cast<float>(tileSide) / atlasH;
        toTile[3][0] = (static_cast<float>(tileX) + 0.5f * static_cast<float>(tileSide)) / atlasW;
        toTile[3][1] = (static_cast<float>(tileY) + 0.5f * static_cast<float>(tileSide)) / atlasH;

        sc.lightSpace = lightProj * lightView;
        sc.atlas = toTile * sc.lightSpace;
        sc.center = center;
        sc.halfExtent = halfExtent;
        sc.texelWorld = texelWorld;
        sc.tile = VkRect2D{.offset = {.x = static_cast<int32_t>(tileX), .y = static_cast<int32_t>(tileY)},
                           .extent = {.width = tileSide, .height = tileSide}};
    }
    activeShadowCascades_ = cascades;
    shadowCenter = shadowCascades_[cascades - 1].center;
    return shadowCascades_[cascades - 1].lightSpace;
}

void Renderer::setupWater1xPass() {
    if (!waterRenderer || !vkCtx) return;
    if (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT) {
        refreshSwimEffectsPass();  // scene continuation pass covers the water here
        return;
    }
    VkImageView depthView = vkCtx->getDepthResolveImageView();
    if (!depthView) {
        // Without a resolved depth buffer the single-sampled water has nothing
        // to depth test against, so it has to stay in the multisampled pass.
        LOG_WARNING("No depth resolve image available - water stays in the MSAA scene pass");
        refreshSwimEffectsPass();
        return;
    }

    waterRenderer->createWater1xPass(vkCtx->getSwapchainFormat(), vkCtx->getDepthFormat());
    waterRenderer->createWater1xFramebuffers(
        vkCtx->getSwapchainImageViews(), depthView, vkCtx->getSwapchainExtent());

    // The spray follows the water into its pass, and this is the first point at
    // which that pass exists - the swim effects were built long before it, back
    // when the only choice was the scene pass.
    refreshSwimEffectsPass();
}

// Re-decide where the spray and the minimap draw, and rebuild whichever one
// the answer moved.
//
// Called between frames only: recreatePipelines() destroys the old pipelines
// outright rather than deferring them, so the wait below is what keeps that
// off a frame in flight. Free when nothing changed, which is every frame but
// a handful, so beginFrame runs it too. The decision depends on whether the
// post-process target exists, and that target is created lazily on the frame
// after the anti-aliasing rebuild that first made the decision. With MSAA and
// FXAA together the rebuild chose the single-sample water pass, the FXAA
// target then moved the water back into the scene pass, and the minimap and
// the spray - still flagged for a pass that no longer ran - were drawn
// nowhere at all. The same happened, in either direction, whenever FXAA was
// toggled in the settings.
void Renderer::refreshSwimEffectsPass() {
    if (!vkCtx) return;
    const bool sprayWasWithWater = swimEffectsDrawWithWater_;
    const bool minimapWasWithWater = minimapDrawsWithWater_;
    const bool glareWasWithWater = glareDrawsWithWater_;
    syncSwimEffectsTargetPass();
    const bool sprayMoved = swimEffects && swimEffectsDrawWithWater_ != sprayWasWithWater;
    const bool minimapMoved = minimap && minimapDrawsWithWater_ != minimapWasWithWater;
    Celestial* celestial = skySystem ? skySystem->getCelestial() : nullptr;
    const bool glareMoved = celestial && (glareDrawsWithWater_ != glareWasWithWater ||
                                          celestial->glarePipelineStale());
    if (!sprayMoved && !minimapMoved && !glareMoved) return;
    vkDeviceWaitIdle(vkCtx->getDevice());
    if (sprayMoved) swimEffects->recreatePipelines();
    if (minimapMoved) minimap->recreatePipelines();
    if (glareMoved) celestial->recreateGlarePipeline();
}

// ========================= Multithreaded Secondary Command Buffers =========================

bool Renderer::createSecondaryCommandResources() {
    if (!vkCtx) return false;
    VkDevice device = vkCtx->getDevice();
    uint32_t queueFamily = vkCtx->getGraphicsQueueFamily();

    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = queueFamily;

    // Create worker command pools (one per worker thread)
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        if (vkCreateCommandPool(device, &poolCI, nullptr, &workerCmdPools_[w]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create worker command pool ", w);
            return false;
        }
    }

    // Create main-thread secondary command pool
    if (vkCreateCommandPool(device, &poolCI, nullptr, &mainSecondaryCmdPool_) != VK_SUCCESS) {
        LOG_ERROR("Failed to create main secondary command pool");
        return false;
    }

    // Allocate secondary command buffers
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = 1;

    // Each concurrently recorded worker secondary owns a dedicated command pool.
    const uint32_t workerSecondaries[] = { SEC_TERRAIN, SEC_WMO, SEC_CHARS, SEC_M2, SEC_POST };
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        allocInfo.commandPool = workerCmdPools_[w];
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[workerSecondaries[w]][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate worker secondary buffer w=", w, " f=", f);
                return false;
            }
        }
    }

    const uint32_t mainSecondaries[] = { SEC_SKY, SEC_SELECTION, SEC_IMGUI };
    for (uint32_t idx : mainSecondaries) {
        allocInfo.commandPool = mainSecondaryCmdPool_;
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[idx][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate main secondary buffer idx=", idx, " f=", f);
                return false;
            }
        }
    }

    parallelRecordingEnabled_ = true;
    LOG_INFO("Multithreaded rendering: ", NUM_WORKERS, " worker threads, ",
             NUM_SECONDARIES, " secondary buffers [ENABLED]");
    return true;
}

void Renderer::destroySecondaryCommandResources() {
    // The doodad worker records into one of these pools.
    syncWorldRecording();
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    vkDeviceWaitIdle(device);

    // Secondary buffers are freed when their pool is destroyed
    for (auto& workerCmdPool : workerCmdPools_) {
        if (workerCmdPool) {
            vkDestroyCommandPool(device, workerCmdPool, nullptr);
            workerCmdPool = VK_NULL_HANDLE;
        }
    }
    if (mainSecondaryCmdPool_) {
        vkDestroyCommandPool(device, mainSecondaryCmdPool_, nullptr);
        mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    }

    for (auto& arr : secondaryCmds_)
        for (auto& cmd : arr)
            cmd = VK_NULL_HANDLE;

    parallelRecordingEnabled_ = false;
}

VkCommandBuffer Renderer::beginSecondary(uint32_t secondaryIndex) {
    uint32_t frame = vkCtx->getCurrentFrame();
    VkCommandBuffer cmd = secondaryCmds_[secondaryIndex][frame];

    VkCommandBufferInheritanceInfo inheritInfo{};
    inheritInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritInfo.renderPass = activeRenderPass_;
    inheritInfo.subpass = 0;
    inheritInfo.framebuffer = activeFramebuffer_;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                    | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    beginInfo.pInheritanceInfo = &inheritInfo;

    VkResult result = vkBeginCommandBuffer(cmd, &beginInfo);
    if (result != VK_SUCCESS) {
        LOG_ERROR("vkBeginCommandBuffer failed for secondary ", secondaryIndex,
                  " frame ", frame, " result=", static_cast<int>(result));
    }
    return cmd;
}

void Renderer::renderM2Models(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, bool withCharacters) {
    // The client keeps every model of the scene - doodads and units alike - in
    // one list sorted by distance, and draws the blended ones far to near. The
    // doodads are drawn here; the characters' opaque batches were drawn with
    // the characters, and their blended ones are offered to the doodads' loop,
    // each before the nearest doodad it stands behind.
    std::vector<CharacterRenderer::BlendedDraw> plan;
    if (withCharacters && characterRenderer) plan = characterRenderer->planBlended(*camera);
    size_t next = 0;
    const M2Renderer::BlendedInterleave among{
        .peek = [&](float& distSq) {
            if (next >= plan.size()) return false;
            distSq = plan[next].distSq;
            return true;
        },
        .draw = [&] {
            characterRenderer->renderBlendedInstance(cmd, perFrameSet, *camera, plan[next++].instanceId);
        }};
    m2Renderer->render(cmd, perFrameSet, *camera, &among);
    // Whatever stands in front of every doodad, or when the doodad pass drew
    // nothing at all.
    while (next < plan.size()) among.draw();
}

void Renderer::setSecondaryViewportScissor(VkCommandBuffer cmd) {
    VkViewport vp{};
    vp.width = static_cast<float>(activeRenderExtent_.width);
    vp.height = static_cast<float>(activeRenderExtent_.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = activeRenderExtent_;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void Renderer::renderReflectionPass() {
    WOWEE_PROFILE_SCOPE("record water reflection", Cpu);
    if (!waterRenderer || !camera || !waterRenderer->hasReflectionPass() || !waterRenderer->hasSurfaces()) return;
    if (!waterRenderer->isEnhancedWater()) return;  // the client's liquid reflects nothing
    if (currentCmd == VK_NULL_HANDLE || !reflPerFrameUBOMapped) return;

    // Select the current frame's pre-bound reflection descriptor set
    // (each frame's set was bound to its own shadow depth view at init).
    uint32_t frame = vkCtx->getCurrentFrame();
    VkDescriptorSet reflDescSet = reflPerFrameDescSet[frame];

    // Reflection pass uses 1x MSAA. Scene pipelines must be render-pass-compatible,
    // which requires matching sample counts. Only render scene into reflection when MSAA is off.
    bool canRenderScene = (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT);

    // Find dominant water height near camera
    const glm::vec3 camPos = camera->getPosition();
    auto waterH = waterRenderer->getDominantWaterHeight(camPos);
    if (!waterH) return;

    float waterHeight = *waterH;

    // Skip reflection if camera is underwater (Z is up)
    if (camPos.z < waterHeight + 0.5f) return;

    // Compute reflected view and oblique projection
    glm::mat4 reflView = WaterRenderer::computeReflectedView(*camera, waterHeight);
    glm::mat4 reflProj = WaterRenderer::computeObliqueProjection(
        camera->getProjectionMatrix(), reflView, waterHeight);

    // Update water renderer's reflection UBO with the reflected viewProj
    waterRenderer->updateReflectionUBO(reflProj * reflView);

    // Fill the reflection per-frame UBO (same as normal but with reflected matrices)
    GPUPerFrameData reflData = currentFrameData;
    reflData.view = reflView;
    reflData.projection = reflProj;
    // Reflected camera position (Z is up)
    glm::vec3 reflPos = camPos;
    reflPos.z = 2.0f * waterHeight - reflPos.z;
    reflData.viewPos = glm::vec4(reflPos, 1.0f);
    // The fog volume is the camera's; its sets here bind the neutral one.
    reflData.volumetricParams = glm::vec4(0.0f);
    reflData.rtParams = glm::vec4(0.0f);
    std::memcpy(reflPerFrameUBOMapped, &reflData, sizeof(GPUPerFrameData));

    // Begin reflection render pass (clears to black; scene rendered if pipeline-compatible)
    if (!waterRenderer->beginReflectionPass(currentCmd)) return;

    if (canRenderScene) {
        // Render scene into reflection texture (sky + terrain + WMO only for perf)
        if (skySystem) {
            auto* reflSkybox = skySystem->getSkybox();
            const float reflTimeOfDay = lightingManager
                ? lightingManager->getTimeOfDayHours()
                : (reflSkybox ? reflSkybox->getTimeOfDay() : 12.0f);
            // The same fields the main sky gets, from the one place that
            // fills them. weatherIntensity stays 0 for the reflection pass
            // (no game handler in scope).
            rendering::SkyParams skyParams = rendering::skyParamsFromLighting(
                reflTimeOfDay, -1.0f, 0.0f,
                lightingManager ? &lightingManager->getLightingParams() : nullptr, false);
            // A flare is an artefact of the lens, so it belongs to the camera
            // and not to what the water is showing it; the glare is drawn
            // over the camera's own picture only (0x009ac400).
            skyParams.sunOcclusion = 1.0f;
            skyParams.moonOcclusion = 1.0f;
            skySystem->render(currentCmd, reflDescSet, *camera, skyParams);
        }
        if (terrainRenderer && terrainEnabled) {
            terrainRenderer->render(currentCmd, reflDescSet, *camera);
        }
        if (wmoRenderer) {
            wmoRenderer->render(currentCmd, reflDescSet, *camera);
        }
    }

    waterRenderer->endReflectionPass(currentCmd);
}

bool Renderer::shadowPassDrawsThisFrame() const {
    // One answer for planShadowPass and for the fog, which samples what it
    // draws: two copies of these conditions could disagree, and a fog
    // dispatched on a frame whose map was not drawn reads whatever the slot
    // was left holding - or, before the first drawn frame, an image that has
    // never left UNDEFINED.
    static const bool skipShadows = (std::getenv("WOWEE_SKIP_SHADOWS") != nullptr);
    if (skipShadows) return false;
    if (passAblation_ && passAblation_->skip(AblationPass::Shadows)) return false;
    if (!vkCtx) return false;
    const uint32_t frame = vkCtx->getCurrentFrame();
    if (frame >= MAX_FRAMES || shadowDepthImage[frame] == VK_NULL_HANDLE ||
        shadowDepthView[frame] == VK_NULL_HANDLE) {
        return false;
    }
    // lightSpaceMatrix is computed at frame start, before updatePerFrameUBO.
    // A zero matrix means the character has no position yet.
    return lightSpaceMatrix != glm::mat4(0.0f) && activeShadowCascades_ >= 1;
}

// The shadow pass is recorded off the main thread.
//
// It cost the main thread some 2.5ms a frame - the doodads' cull and sort
// most of it - inside beginFrame, ahead of everything that reads the map, and
// so it could only be recorded there. It is recorded into the frame's
// prologue now (VkContext::getFramePrologue), a second command buffer that is
// submitted ahead of this one in the same batch: whatever goes into it
// executes first, whenever it is recorded, so the map is drawn and readable
// before the fog, the reflection and the scene sample it however late in the
// frame its recording finishes. A secondary executed into this buffer would
// not do: it has to be recorded before the command that executes it, and that
// command would have to sit here in beginFrame.
//
// beginFrame plans the pass (planShadowPass): it settles the layout, the
// cascades and the timestamps, which the fog recorded right after it needs.
// renderWorld hands the plan to a frame worker once the world's preparation is
// done, and the worker records it while the interface runs, beside the doodad
// worker. Without a world, or with the world drawn inline, endFrame records
// it on the main thread instead, exactly as beginFrame used to.
//
// What the worker reads is fenced the way the doodad worker's is (see
// launchShadowWorker): the getters of every renderer it draws wait for it.

void Renderer::planShadowPass() {
    shadowPlan_ = ShadowPlan();
    if (shadowDepthImage[0] == VK_NULL_HANDLE) return;
    if (currentCmd == VK_NULL_HANDLE) return;
    // Shadows off still runs the whole pass, casters and all: the shaders
    // stop reading the map (shadowParams.x), and that is the only difference.
    // Skipping the casters lost the device within a second - something they
    // record each frame is read later in the frame, and it was not found -
    // and returning here instead left the image untransitioned while it
    // stayed bound for sampling. The cost is a shadow pass nobody sees.

    // Shadows render every frame - throttling causes visible flicker on player/NPCs

    // Before the character has a position, or with the pass switched off for
    // measuring (WOWEE_SKIP_SHADOWS, the ablation), there is nothing to draw.
    if (!shadowPassDrawsThisFrame()) return;
    const uint32_t frame = vkCtx->getCurrentFrame();

    ShadowPlan& plan = shadowPlan_;
    plan.cmd = vkCtx->getFramePrologue();
    if (plan.cmd == VK_NULL_HANDLE) return;
    plan.frame = frame;
    plan.image = shadowDepthImage[frame];
    plan.view = shadowDepthView[frame];
    plan.framebuffer = shadowFramebuffer[frame];
    plan.renderPass = shadowRenderPass;
    plan.oldLayout = shadowDepthLayout_[frame];
    plan.atlas = {.width = shadowAtlasWidth(shadowCascadeCount_, SHADOW_MAP_SIZE),
                  .height = shadowAtlasHeight(shadowCascadeCount_, SHADOW_MAP_SIZE)};
    // Copied, and the renderers' pointers with them: a worker reads nothing of
    // this object's that the main thread can change while it records.
    plan.cascadeCount = std::min(activeShadowCascades_, kMaxShadowCascades);
    for (int c = 0; c < plan.cascadeCount; ++c) plan.cascades[c] = shadowCascades_[c];
    plan.terrain = terrainRenderer.get();
    plan.wmo = wmoRenderer.get();
    plan.m2 = m2Renderer.get();
    plan.characters = characterRenderer.get();
    plan.marks = vkCtx->gpuReserveMarks(kMaxShadowCascades + 1);

    // How recordShadowPass begins the pass, one way or the other.
    //
    // The first pass converted to dynamic rendering, and the one with least
    // to lose by it: one attachment, no colour, no resolve, a single
    // begin/end, and pipelines nothing else shares. Its render pass declared
    // an EXTERNAL->0 dependency covering the same fragment-read to
    // depth-write hazard that barrier 1 already covers explicitly, so
    // nothing is lost by dropping the implicit half - the layout it wants is
    // the layout b1 leaves it in.
    plan.dynamicRendering = vkCtx->useDynamicRendering();
    // Said once, at warning level, because a bug report arrives with a
    // warnings-only log and "are the shadows drawn the new way" is the first
    // question this change makes anyone ask. A line here answers it without
    // a second run.
    static bool saidWhichPath = false;
    if (!saidWhichPath) {
        saidWhichPath = true;
        LOG_WARNING("Shadow pass records with ",
                    plan.dynamicRendering ? "vkCmdBeginRendering" : "a VkRenderPass",
                    ", into the frame's prologue on ",
                    parallelRecordingEnabled_ ? "a frame worker while in the world"
                                              : "the main thread");
    }

    // The layout the map is in once the prologue has run, which is before
    // anything in this frame's own buffer - the fog recorded a moment from
    // now included - reads it.
    shadowDepthLayout_[frame] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    plan.pending = true;
}

void Renderer::launchShadowWorker() {
    if (!shadowPlan_.pending) return;
    const ShadowPlan plan = shadowPlan_;
    shadowPlan_.pending = false;
    // What it shares, and why none of it races:
    //
    // - The doodad worker records the same model and character renderers at
    //   the same time. The model renderer's shadow state - the packed cull
    //   copy, the candidate and caster lists, the shadow instance buffer,
    //   its texture-set pool and cache - is touched by nothing but
    //   renderShadow and beginShadowFrame; the main pass writes only its own
    //   lists and each instance's cull verdict, fields the shadow pass does
    //   not read. Both only read the instance table, the models and their
    //   textures. The character renderer's two passes share the bone
    //   buffers they both copy the pose into, so renderShadow takes the same
    //   lock render() does.
    // - The main thread, through the interface, changes nothing either of
    //   them reads except through the getters, which wait (syncWorldRecording
    //   for the model and character renderers, syncShadowRecording for the
    //   terrain and the buildings). The const query accessors that do not
    //   wait read positions and bounds; the shadow pass writes none.
    // - Everything of the Renderer's own it needs is in the plan, by value.
    // - The prologue is recorded by this worker alone, from a pool of its own,
    //   and endFrame submits it only after finishRenderWorld has joined it.
    shadowFuture_ = core::ThreadPool::frameWorkers().submit([this, plan]() {
        WOWEE_PROFILE_SCOPE("worker: shadows", Worker);
        recordShadowPass(plan, true);
    });
    shadowWorkerInFlight_.store(true, std::memory_order_release);
}

void Renderer::joinShadowWorker() const {
    // The main thread only, for the reason joinM2Worker gives.
    if (std::this_thread::get_id() != mainThreadId_) return;
    if (!shadowWorkerInFlight_.load(std::memory_order_acquire)) return;
    std::optional<core::ProfileScope> joinScope;
    if (core::FrameProfiler::enabled()) {
        joinScope.emplace("join shadow worker", core::ProfileKind::Cpu);
    }
    try { if (shadowFuture_.valid()) shadowFuture_.get(); }
    catch (const std::exception& e) { LOG_ERROR("Shadow render worker: ", e.what()); }
    shadowWorkerInFlight_.store(false, std::memory_order_release);
}

void Renderer::flushShadowPass() {
    if (!shadowPlan_.pending) return;
    const ShadowPlan plan = shadowPlan_;
    shadowPlan_.pending = false;
    WOWEE_PROFILE_SCOPE("record shadows", Cpu);
    recordShadowPass(plan, false);
}

void Renderer::recordShadowPass(const ShadowPlan& plan, bool onWorker) {
    ZoneScopedN("Renderer::recordShadowPass");
    const VkCommandBuffer cmd = plan.cmd;
    // The stages below are filed with the thread that runs them, so the
    // profile shows them under the render workers when a worker does.
    const core::ProfileKind kind = onWorker ? core::ProfileKind::Worker : core::ProfileKind::Cpu;
    // Written into the indices beginFrame reserved, in the order they draw.
    uint32_t markIndex = 0;
    const auto mark = [&](uint32_t index, const char* label) {
        vkCtx->checkpoint(cmd, label);
        if (plan.marks != VkContext::kNoGpuMark) vkCtx->gpuMarkAt(cmd, plan.marks + index, label);
    };

    // Barrier 1: transition this frame's shadow map into writable depth layout.
    VkImageMemoryBarrier2 b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b1.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    b1.oldLayout = plan.oldLayout;
    b1.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.srcAccessMask = (plan.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? VK_ACCESS_SHADER_READ_BIT
        : 0;
    b1.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b1.image = plan.image;
    b1.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    // The fog's compute pass reads the map as well as the fragment shaders.
    // Those reads are the previous use of this slot's map, two frames back,
    // and a barrier orders against everything submitted before it on the
    // queue, so being in another command buffer than they were changes
    // nothing.
    VkPipelineStageFlags srcStage = (plan.oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
        : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    b1.srcStageMask = srcStage;
    VkDependencyInfo b1Dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b1Dep.dependencyFlags = 0;
    b1Dep.imageMemoryBarrierCount = 1;
    b1Dep.pImageMemoryBarriers = &b1;
    cmdPipelineBarrier2(cmd, b1Dep);

    if (plan.dynamicRendering) {
        VkRenderingAttachmentInfo depthAttach{};
        depthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttach.imageView = plan.view;
        depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttach.clearValue.depthStencil = {.depth = 1.0f, .stencil = 0};

        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = plan.atlas};
        renderInfo.layerCount = 1;
        renderInfo.pDepthAttachment = &depthAttach;
        vkCmdBeginRendering(cmd, &renderInfo);
    } else {
        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass = plan.renderPass;
        rpInfo.framebuffer = plan.framebuffer;
        rpInfo.renderArea = {.offset = {.x = 0, .y = 0}, .extent = plan.atlas};
        VkClearValue clear{};
        clear.depthStencil = {.depth = 1.0f, .stencil = 0};
        rpInfo.clearValueCount = 1;
        rpInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Phase 7/8: render shadow casters, once per cascade into its own tile.
    //
    // What the casters keep for the frame - descriptor pools, the instance and
    // indirect buffers they write their draws into, the bones - is reset here,
    // once, and the cascades after the first append to it. Reset per call, the
    // second cascade's writes would replace what the first one's draws read
    // when the GPU gets to them.
    if (plan.terrain) plan.terrain->beginShadowFrame(plan.frame);
    if (plan.m2) plan.m2->beginShadowFrame(plan.frame);
    if (plan.characters) plan.characters->beginShadowFrame(plan.frame);
    // Largest first: each cascade lies inside the one before it, so the M2
    // renderer culls the first one against every instance and the rest
    // against that list (M2Renderer::shadowCandidates_).
    for (int c = plan.cascadeCount - 1; c >= 0; --c) {
        const ShadowCascade& cascade = plan.cascades[c];
        VkViewport vp{.x = static_cast<float>(cascade.tile.offset.x),
                      .y = static_cast<float>(cascade.tile.offset.y),
                      .width = static_cast<float>(cascade.tile.extent.width),
                      .height = static_cast<float>(cascade.tile.extent.height),
                      .minDepth = 0.0f, .maxDepth = 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &cascade.tile);

        const float cullRadius = cascade.halfExtent * 1.35f;
        if (plan.terrain) {
            const core::ProfileScope scope("shadow: terrain", kind);
            plan.terrain->renderShadow(cmd, cascade.lightSpace, cascade.center, cullRadius);
        }
        if (plan.wmo) {
            const core::ProfileScope scope("shadow: wmo", kind);
            plan.wmo->renderShadow(cmd, cascade.lightSpace, cascade.center, cullRadius);
        }
        if (plan.m2) {
            const core::ProfileScope scope("shadow: m2", kind);
            plan.m2->renderShadow(cmd, cascade.lightSpace, cascade.center, cullRadius);
        }
        if (plan.characters) {
            const core::ProfileScope scope("shadow: characters", kind);
            plan.characters->renderShadow(cmd, cascade.lightSpace, cascade.center, cullRadius);
        }
        // One mark per cascade: the far one covers the most ground and is
        // usually the expensive one, which a single shadow total cannot say.
        // The first also carries the clear of the atlas.
        static constexpr const char* kCascadeLabels[kMaxShadowCascades] = {
            "shadow cascade 0 (near)", "shadow cascade 1", "shadow cascade 2 (far)"};
        mark(markIndex++, kCascadeLabels[c]);
    }

    if (plan.dynamicRendering) {
        vkCmdEndRendering(cmd);
    } else {
        vkCmdEndRenderPass(cmd);
    }

    // Barrier 2: DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier2 b2{};
    b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b2.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    // Compute too: the volumetric fog samples it right after this pass.
    b2.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.image = plan.image;
    b2.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    VkDependencyInfo b2Dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b2Dep.dependencyFlags = 0;
    b2Dep.imageMemoryBarrierCount = 1;
    b2Dep.pImageMemoryBarriers = &b2;
    cmdPipelineBarrier2(cmd, b2Dep);
    // The end of the pass, outside it: what the cascades add up to even on a
    // driver that will not split a render pass between its marks.
    mark(kMaxShadowCascades, "shadows (pass end)");
}

VkImageView Renderer::getNeutralRtLightingView() const {
    return rtLighting_ ? rtLighting_->neutralView() : VK_NULL_HANDLE;
}

void Renderer::setRtLightingMode(int mode) {
    if (!rtLighting_) return;
    rtLighting_->setMode(static_cast<RtLighting::Mode>(std::clamp(mode, 0, 3)));
}

void Renderer::writeRtLightingBindings() {
    if (!rtLighting_ || !vkCtx) return;
    // RtLighting::prepare has waited for the device whenever it reports a
    // change, so neither slot's set is in use.
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorImageInfo info[2]{};
        info[0].imageView = rtLighting_->lightViewForSlot(i);
        info[1].imageView = rtLighting_->giViewForSlot(i);
        info[0].imageLayout = info[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t b = 0; b < 2; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = perFrameDescSets[i];
            writes[b].dstBinding = 3 + b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &info[b];
        }
        vkUpdateDescriptorSets(vkCtx->getDevice(), 2, writes, 0, nullptr);
    }
}

glm::vec3 Renderer::sunTravelDirection() const {
    // Away from the sun as the sky draws it (the sun curve, 0x007eecc0), so
    // the shadows and the shafts through the mist fall from where the sun is
    // seen. The light the surfaces are shaded by keeps its own direction, the
    // light's (Light.dbc), which can stand well apart from it - the shadows
    // used to follow that one, and lay across the sun the player looked at.
    // Below the horizon there is no sun to follow, and the light's direction
    // stands in, as before.
    if (lightingManager) {
        const auto& lp = lightingManager->getLightingParams();
        const float sunLenSq = glm::dot(lp.sunDir, lp.sunDir);
        if (sunLenSq > 1e-6f && lp.sunDir.z > 0.0f) {
            return -lp.sunDir * glm::inversesqrt(sunLenSq);
        }
        const float ldirLenSq = glm::dot(lp.directionalDir, lp.directionalDir);
        if (ldirLenSq > 1e-6f) {
            return -lp.directionalDir * glm::inversesqrt(ldirLenSq);
        }
    }
    return glm::normalize(glm::vec3(-0.3f, -0.7f, -0.6f));
}

VkExtent2D Renderer::sceneRenderExtent() const {
    if (postProcessPipeline_ && postProcessPipeline_->getSceneFramebuffer() != VK_NULL_HANDLE) {
        return postProcessPipeline_->getSceneRenderExtent();
    }
    return vkCtx->getSwapchainExtent();
}

void Renderer::recordRtLighting(VkImage sceneDepth, VkExtent2D sceneExtent, bool depthIsMsaa) {
    if (!rtLighting_ || !rtLighting_->active() || !camera) return;
    rtRecordedThisFrame_ = true;
    RtLighting::FrameInputs in{};
    in.viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    in.cameraPos = camera->getPosition();
    in.sunDir = -glm::vec3(currentFrameData.lightDir);
    in.sunColor = glm::vec3(currentFrameData.lightColor);
    in.skyColor = glm::vec3(currentFrameData.ambientColor);
    in.sunUp = in.sunDir.z > -0.05f;
    if (wmoRenderer) wmoRenderer->syncRtScene();
    if (m2Renderer) m2Renderer->syncRtScene();
    rtLighting_->record(currentCmd, sceneDepth, sceneExtent, depthIsMsaa, in);
    vkCtx->gpuMark(currentCmd, "rt_lighting");
}

VkImageView Renderer::getNeutralFogVolumeView() const {
    return volumetricFog_ ? volumetricFog_->getNeutralView() : VK_NULL_HANDLE;
}

void Renderer::setVolumetricFogQuality(int quality) {
    if (!volumetricFog_) return;
    volumetricFog_->setQuality(static_cast<VolumetricFog::Quality>(std::clamp(quality, 0, 3)));
}

void Renderer::writeFogVolumeBindings() {
    if (!volumetricFog_ || !vkCtx) return;
    // applyPendingQuality has already waited for the device, so neither
    // slot's set is in use by a frame still in flight.
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorImageInfo fogImgInfo{};
        fogImgInfo.imageView = volumetricFog_->getVolumeView(i);
        fogImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = perFrameDescSets[i];
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &fogImgInfo;
        vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
    }
}

uint32_t Renderer::maxShadowMapSize(int cascades) const {
    cascades = std::clamp(cascades, 1, kMaxShadowCascades);
    // The tile is held to 16384 alone and to 8192 with company: past that the
    // atlas is more memory than a player choosing "sharper" expects to pay.
    uint32_t side = cascades > 1 ? kMaxCascadeTileSize : kMaxShadowMapSize;
    if (vkCtx) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(vkCtx->getPhysicalDevice(), &props);
        // The atlas has to be made, drawn into as a framebuffer and covered by
        // the viewport; the spec only promises 4096 for each. It is two tiles
        // across with two or more cascades, and two down with three.
        const VkPhysicalDeviceLimits& lim = props.limits;
        const uint32_t across = std::min({lim.maxImageDimension2D, lim.maxFramebufferWidth,
                                          lim.maxViewportDimensions[0]});
        const uint32_t down = std::min({lim.maxImageDimension2D, lim.maxFramebufferHeight,
                                        lim.maxViewportDimensions[1]});
        side = std::min({side, across / (cascades > 1 ? 2u : 1u), down / (cascades > 2 ? 2u : 1u)});
    }
    // A power of two, as every size the map is built at is.
    uint32_t pow2 = 512;
    while (pow2 * 2 <= side) pow2 *= 2;
    return pow2;
}

void Renderer::setShadowResolutionScale(int step) {
    shadowResolutionStep_ = std::clamp(step, 0, 3);
    requestShadowAtlas();
}

void Renderer::setShadowCascadeCount(int count) {
    shadowCascadesWanted_ = std::clamp(count, 1, kMaxShadowCascades);
    requestShadowAtlas();
}

void Renderer::requestShadowAtlas() {
    // Never below the quality level's size where the device can draw the
    // atlas at it: alone that is at most 4096, which every device has to
    // support. Two cascades side by side at 4096 are 8192 across, which a
    // device need not, and then the tile gives way rather than the cascades.
    pendingShadowMapSize_ = std::min(shadowMapBaseSize_ << shadowResolutionStep_,
                                     maxShadowMapSize(shadowCascadesWanted_));
    pendingShadowCascades_ = shadowCascadesWanted_;
}

void Renderer::writeShadowMapBindings() {
    // The views are new, so every set naming the old ones is written again:
    // both frame slots' sets and the reflection's, binding 1 each. The
    // character preview binds a dummy view of its own and is not one of them.
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorImageInfo shadowImgInfo{};
        // sampler is ignored: binding 1 declares it immutable in the layout.
        shadowImgInfo.imageView = shadowDepthView[i];
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[2]{};
        const VkDescriptorSet sets[2] = {perFrameDescSets[i], reflPerFrameDescSet[i]};
        uint32_t count = 0;
        for (VkDescriptorSet set : sets) {
            if (set == VK_NULL_HANDLE) continue;
            writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[count].dstSet = set;
            writes[count].dstBinding = 1;
            writes[count].descriptorCount = 1;
            writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].pImageInfo = &shadowImgInfo;
            ++count;
        }
        if (count > 0) vkUpdateDescriptorSets(vkCtx->getDevice(), count, writes, 0, nullptr);
    }
    // The fog's inject pass reads the map through a set of its own.
    if (volumetricFog_) volumetricFog_->setShadowViews(shadowDepthView);
}

void Renderer::applyPendingShadowMapSize() {
    if (pendingShadowCascades_ == 0 || !vkCtx) return;
    const uint32_t wanted = pendingShadowMapSize_;
    const int wantedCascades = pendingShadowCascades_;
    pendingShadowMapSize_ = 0;
    pendingShadowCascades_ = 0;
    // Nothing to remake when the atlas is already this.
    if (wanted == SHADOW_MAP_SIZE && wantedCascades == shadowCascadeCount_) return;
    // Nor before the per-frame resources exist: they are built from these.
    if (shadowRenderPass == VK_NULL_HANDLE) {
        SHADOW_MAP_SIZE = wanted;
        shadowCascadeCount_ = wantedCascades;
        return;
    }

    // Both slots' maps are bound in sets a frame still in flight may read,
    // and one may be mid-write - so nothing goes until the device is idle,
    // as for the fog's volumes and the ray traced lighting's images.
    vkDeviceWaitIdle(vkCtx->getDevice());
    const uint32_t previous = SHADOW_MAP_SIZE;
    const int previousCascades = shadowCascadeCount_;
    destroyShadowMapImages();

    // The larger sizes are a lot of memory, and a failed allocation here
    // must not leave the world without a map to sample. The tile that was
    // built a moment ago is tried next, then smaller again, and at the
    // smallest a single cascade.
    uint32_t side = wanted;
    int cascades = wantedCascades;
    bool built = false;
    for (;;) {
        SHADOW_MAP_SIZE = side;
        shadowCascadeCount_ = cascades;
        if (createShadowMapImages()) { built = true; break; }
        destroyShadowMapImages();
        if (side > previous) side = std::min(previous, maxShadowMapSize(cascades));
        else if (side > 512) side /= 2;
        else if (cascades > 1) cascades = 1;
        else break;
        LOG_WARNING("Shadow map: ", shadowCascadeCount_, " cascade(s) of ", SHADOW_MAP_SIZE,
                    " could not be made - trying ", cascades, " of ", side);
    }
    if (!built) {
        // Not even 512 a side: the device is out of memory, and more than the
        // shadows will fail with it. The shadow pass skips itself without an
        // image; said at error level, since this is what the log has to show.
        LOG_ERROR("Shadow map: no size could be made, down to 512x512");
        // The fog's sets still name the maps just destroyed. Handed the empty
        // views, it switches itself off rather than sample them.
        if (volumetricFog_) volumetricFog_->setShadowViews(shadowDepthView);
        return;
    }
    // computeLightSpaceMatrix lays the cascades out in the atlas and snaps
    // them to the texel of whatever SHADOW_MAP_SIZE is, and updatePerFrameUBO
    // hands the shaders the tiles, both each frame - so the views are all
    // that is left to hand on.
    writeShadowMapBindings();
    LOG_WARNING("Shadow map: rebuilt as ", shadowCascadeCount_, " cascade(s) of ", SHADOW_MAP_SIZE,
                " in a ", shadowAtlasWidth(shadowCascadeCount_, SHADOW_MAP_SIZE), "x",
                shadowAtlasHeight(shadowCascadeCount_, SHADOW_MAP_SIZE), " atlas (was ",
                previousCascades, " of ", previous, ")");
}

float Renderer::volumetricFogExtinction() const {
    // Per yard, at and below the layer, near the camera. At this a sixth of
    // the light is gone over the first hundred yards and two fifths across
    // the whole volume, which thins its air out by 400 - a haze, not a wall,
    // before the slider scales it - while a ten-yard shaft near the sun still
    // lifts what is behind it by a tenth or more.
    constexpr float kBaseExtinction = 0.002f;
    float extinction = kBaseExtinction * volumetricFogDensity_;

    if (lightingManager) {
        const auto& lp = lightingManager->getLightingParams();
        // The zone's own opinion, read off how close its authored fog comes
        // in: Duskwood's ends at 525 yards and gets most of the mist, an
        // open plain's ends far out and gets less. The player's fog slider
        // divides those distances, so it is multiplied back out to reach the
        // zone's number rather than the slider's.
        const float strength = lightingManager->getFogStrength();
        if (strength > 0.001f && lp.fogEnd > 1.0f) {
            const float authoredEnd = lp.fogEnd * strength;
            extinction *= glm::clamp(900.0f / authoredEnd, 0.6f, 2.0f);
        }
        // Morning mist: thickest around half past six, gone by nine.
        const float hours = lightingManager->getTimeOfDayHours();
        const float dawn = 1.0f - glm::smoothstep(0.0f, 2.5f, std::abs(hours - 6.5f));
        extinction *= 1.0f + 0.8f * dawn;
    }

    if (weather) {
        const float w = glm::clamp(weather->getIntensity(), 0.0f, 1.0f);
        switch (weather->getWeatherType()) {
            case Weather::Type::RAIN:  extinction *= 1.0f + 1.2f * w; break;
            case Weather::Type::SNOW:  extinction *= 1.0f + 0.8f * w; break;
            default: break;
        }
    }

    // Indoors the outdoor air mostly stays outside. Not all of it: a hall
    // with torches in it should still show their glow.
    if (cameraController && cameraController->isInsideInteriorWMO()) extinction *= 0.35f;
    return extinction;
}

void Renderer::renderVolumetricFog() {
    ZoneScopedN("Renderer::renderVolumetricFog");
    WOWEE_PROFILE_SCOPE("record volumetric fog", Cpu);
    if (!volumetricThisFrame_ || !volumetricFog_ || !camera || currentCmd == VK_NULL_HANDLE) return;
    const uint32_t frame = vkCtx->getCurrentFrame();
    // The inject pass samples this slot's shadow map, so not before the shadow
    // pass has left it readable: at the login screen, before the player has a
    // position, it has never been drawn and is still UNDEFINED, which the
    // validation layer reports for any dispatch that binds it. Skipping leaves
    // this slot's volume as it was - clear air, until the first real frame -
    // and the shaders read that.
    if (shadowDepthLayout_[frame] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    const float dt = std::max(lastDeltaTime_, 0.0f);

    // The ground the mist lies on: the terrain under the player, or the
    // player's own feet where those are lower - a cave, a city under a
    // mountain - so the layer is never a ceiling over them. Chased over a
    // second or two so a step off a ledge does not lift the whole bank of
    // mist with it; a teleport snaps it, and drops last frame's air too.
    float ground = characterPosition.z;
    if (terrainManager) {
        if (auto h = terrainManager->getHeightAt(characterPosition.x, characterPosition.y)) {
            ground = std::min(ground, *h);
        }
    }
    if (!fogLayerBaseValid_ || std::abs(ground - fogLayerBase_) > 150.0f) {
        fogLayerBase_ = ground;
        fogLayerBaseValid_ = true;
        volumetricFog_->resetHistory();
    } else {
        fogLayerBase_ += (ground - fogLayerBase_) * (1.0f - std::exp(-dt / 1.5f));
    }

    const float target = volumetricFogExtinction();
    if (fogExtinction_ < 0.0f) fogExtinction_ = target;
    fogExtinction_ += (target - fogExtinction_) * (1.0f - std::exp(-dt / 1.0f));

    VolumetricFog::FrameInputs in;
    in.sunTravel = sunTravelDirection();
    in.view = currentFrameData.view;
    in.projection = currentFrameData.projection;
    in.cameraPos = glm::vec3(currentFrameData.viewPos);
    in.time = globalTime;
    in.density = fogExtinction_;
    in.layerBase = fogLayerBase_;
    volumetricFog_->record(currentCmd, frame, perFrameDescSets[frame], in);
    if (vkCtx) vkCtx->gpuMark(currentCmd, "volumetric fog");

    // What it was built from, every few seconds, for the report that says the
    // fog is too thick or missing: INFO, so it costs nothing unless asked for.
    static double lastFogLog = 0.0;
    if (globalTime - lastFogLog > 5.0) {
        lastFogLog = globalTime;
        LOG_INFO("volumetricFog: extinction=", fogExtinction_, "/yd (target ", target,
                 ") layerBase=", fogLayerBase_);
    }
}

void Renderer::loadWmoAreaRows() {
    if (wmoAreaRowsLoaded_ || !wmoRenderer) return;
    auto* assetManager = core::Application::getInstance().getAssetManager();
    if (!assetManager) return;
    wmoAreaRowsLoaded_ = true;
    const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("WMOAreaTable")
                                                        : nullptr;
    if (!layout) return;
    const uint32_t cols[5] = {layout->tryField("WMOID"), layout->tryField("NameSetID"),
                              layout->tryField("WMOGroupID"), layout->tryField("Flags"),
                              layout->tryField("AreaTableID")};
    const uint32_t nameCol = layout->tryField("AreaName");
    auto data = assetManager->readFile("DBFilesClient\\WMOAreaTable.dbc");
    pipeline::DBCFile dbc;
    if (data.empty() || !dbc.load(data)) return;
    for (uint32_t c : cols) {
        if (c >= dbc.getFieldCount()) return;
    }
    const bool haveNames = nameCol < dbc.getFieldCount();
    // The name ends the record - its locale block and that block's flags,
    // eight locales before TBC and sixteen since - in every layout given for
    // this table. A file it does not end is not laid out as the layout says,
    // and its flags would put the minimap indoors at random.
    if (haveNames && nameCol + 9 != dbc.getFieldCount() && nameCol + 17 != dbc.getFieldCount()) {
        LOG_WARNING("WMOAreaTable.dbc has ", dbc.getFieldCount(), " fields, not what its layout says - not read");
        return;
    }
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        minimap_indoor::AreaRow row;
        row.flags = dbc.getUInt32(i, cols[3]);
        row.areaId = dbc.getUInt32(i, cols[4]);
        if (haveNames) row.name = dbc.getString(i, nameCol);
        wmoAreaRows_[minimap_indoor::areaKey(dbc.getUInt32(i, cols[0]), dbc.getUInt32(i, cols[1]),
                                             static_cast<int32_t>(dbc.getUInt32(i, cols[2])))] =
            std::move(row);
    }
    LOG_INFO("Loaded WMOAreaTable.dbc: ", wmoAreaRows_.size(), " rows");
}

void Renderer::loadScreenEffectRows() {
    if (screenEffectRowsLoaded_) return;
    auto* assetManager = core::Application::getInstance().getAssetManager();
    if (!assetManager) return;
    screenEffectRowsLoaded_ = true;
    const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ScreenEffect")
                                                        : nullptr;
    if (!layout) return;
    const uint32_t effectCol = layout->tryField("Effect");
    const uint32_t paramCol = layout->tryField("Param0");
    const uint32_t lightCol = layout->tryField("LightParametersID");
    const uint32_t ambienceCol = layout->tryField("SoundAmbienceID");
    const uint32_t musicCol = layout->tryField("ZoneMusicID");
    auto data = assetManager->readFile("DBFilesClient\\ScreenEffect.dbc");
    pipeline::DBCFile dbc;
    if (data.empty() || !dbc.load(data) || effectCol >= dbc.getFieldCount() ||
        paramCol + 3 >= dbc.getFieldCount() || lightCol >= dbc.getFieldCount()) {
        LOG_WARNING("ScreenEffect.dbc not read - a ghost keeps the death effect, nothing else");
        return;
    }
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        screen_effect::Row row;
        row.id = dbc.getUInt32(i, 0);
        row.kind = static_cast<screen_effect::Kind>(dbc.getUInt32(i, effectCol));
        for (uint32_t p = 0; p < 4; ++p) row.params[p] = static_cast<int32_t>(dbc.getUInt32(i, paramCol + p));
        row.lightOverride = dbc.getUInt32(i, lightCol);
        if (ambienceCol < dbc.getFieldCount()) row.soundAmbience = dbc.getUInt32(i, ambienceCol);
        if (musicCol < dbc.getFieldCount()) row.zoneMusic = dbc.getUInt32(i, musicCol);
        screenEffectRows_[row.id] = row;
    }
    LOG_INFO("Loaded ScreenEffect.dbc: ", screenEffectRows_.size(), " rows");
}

void Renderer::updateScreenEffect(float deltaTime) {
    namespace se = screen_effect;
    loadScreenEffectRows();
    auto* gh = core::Application::getInstance().getGameHandler();
    std::vector<se::AuraEffects> slots;
    bool ghost = false;
    bool inArena = false;
    uint32_t bytes2 = 0, bytes3 = 0, fakeDrunk = 0;
    screenEffectHavePlayer_ = false;
    if (gh) {
        ghost = gh->isPlayerGhost();
        inArena = ghost && gh->isArenaMap(gh->getCurrentMapId());
        // The aura slots and their spells' three effects (0x004f88b0 reads
        // EffectApplyAuraName and EffectMiscValue).
        for (const auto& aura : gh->getPlayerAuras()) {
            se::AuraEffects a;
            if (!aura.isEmpty()) {
                gh->getSpellName(aura.spellId);  // fills the cache
                auto it = gh->spellNameCacheRef().find(aura.spellId);
                if (it != gh->spellNameCacheRef().end()) {
                    a.spellId = aura.spellId;
                    for (int i = 0; i < 3; ++i) {
                        a.auraType[i] = it->second.effectAuraIds[i];
                        a.miscValue[i] = it->second.effectMiscValues[i];
                    }
                }
            }
            slots.push_back(a);
        }
        if (auto player = gh->getEntityManager().getEntity(gh->getPlayerGuid())) {
            screenEffectHavePlayer_ = true;
            bytes2 = player->getField(game::fieldIndex(game::UF::PLAYER_FIELD_BYTES2));
            bytes3 = player->getField(game::fieldIndex(game::UF::PLAYER_BYTES_3));
            fakeDrunk = player->getField(game::fieldIndex(game::UF::PLAYER_FAKE_INEBRIATION));
        }
    }
    const uint32_t id = se::chooseRow(slots, ghost, inArena, bytes2);
    const se::Row* row = nullptr;
    if (id != 0) {
        auto it = screenEffectRows_.find(id);
        if (it != screenEffectRows_.end()) row = &it->second;
    }
    // Without the table a ghost still gets what row 1 holds: the death
    // effect over the death light (slot 4).
    static const se::Row kDeathRow{.id = se::kRowDeath, .kind = se::Kind::Death, .params = {0, 0, 0, 0},
                                   .lightOverride = 4};
    if (!row && id == se::kRowDeath && screenEffectRows_.empty()) row = &kDeathRow;
    screenEffectState_.select(row);
    screenEffectState_.advance(deltaTime);
    screenEffectLightOverride_ = row && row->lightOverride < 8 ? static_cast<int>(row->lightOverride) : -1;
    // Its sounds take the top sound slot (0x004f7020 -> 0x004c8fa0).
    screenEffectAmbience_ = row ? row->soundAmbience : 0;
    screenEffectZoneMusic_ = row ? row->zoneMusic : 0;
    screenEffectDrunk_ = se::drunkAmount(bytes3, fakeDrunk);
}

void Renderer::recordScreenEffects() {
    if (!screenEffects_ || currentCmd == VK_NULL_HANDLE) return;
    namespace se = screen_effect;
    ScreenEffects::FrameInputs in;
    const auto& images = vkCtx->getSwapchainImages();
    bool draw = false;
    if (worldDrawnThisFrame_ && lightingManager && currentImageIndex < images.size()) {
        const float lightGlow = lightingManager->getLightingParams().glow;
        switch (screenEffectState_.kind()) {
            case se::Kind::Death:
                // ffxDeath in the glow's place (0x007ea260).
                in.mode = ScreenEffects::Mode::Death;
                in.glow = screenGlowEnabled_ ? lightGlow : 0.0f;
                draw = true;
                break;
            case se::Kind::NetherWorld: {
                in.mode = ScreenEffects::Mode::Nether;
                const auto& field = screenEffectState_.nether();
                for (int k = 0; k < se::NetherField::kPoints; ++k) in.netherValues[k] = field.pointValue(k);
                // The world's x axis in the view (the view matrix's first row
                // to the client), its z turned to the client's forward.
                if (camera) {
                    const glm::mat4& v = camera->getViewMatrix();
                    in.netherAngle = field.angle(v[0][0], v[0][1], -v[0][2]);
                }
                in.netherFade = screenEffectState_.netherFade();
                draw = true;
                break;
            }
            case se::Kind::Special: {
                in.mode = ScreenEffects::Mode::Fog;
                const uint32_t c = screenEffectState_.fogColour();
                in.fogColour = glm::vec3(static_cast<float>((c >> 16) & 0xFFu), static_cast<float>((c >> 8) & 0xFFu),
                                         static_cast<float>(c & 0xFFu)) / 255.0f;
                in.fogDecay = screenEffectState_.fogDecay();
                in.fogNoiseRow = screenEffectState_.fogNoiseRow();
                in.fogDesaturate = screenEffectState_.fogDesaturate();
                in.fogBrighten = screenEffectState_.fogBrighten();
                draw = true;
                break;
            }
            default: {
                // ffxGlow (0x004f8770): the light's glow, and the blend toward
                // the blur - the camera in liquid, with the wave, or drunk.
                if (!screenGlowEnabled_) break;
                const se::GlowBlend g = se::glowBlend(lightGlow, screenEffectHavePlayer_, screenEffectDrunk_,
                                                      cameraInLiquid_);
                in.mode = ScreenEffects::Mode::Glow;
                in.glow = static_cast<float>(g.glow) / 255.0f;
                in.blend = static_cast<float>(g.blend) / 255.0f;
                in.wave = g.wave;
                in.timeMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                draw = true;
                break;
            }
        }
        if (minimap && minimap->isEnabled() && window && window->getWidth() > 0 && window->getHeight() > 0) {
            in.keepRect = minimap->screenUvRect(window->getWidth(), window->getHeight());
        }
    }
    screenEffects_->record(currentCmd, vkCtx->getCurrentFrame(),
                           draw && currentImageIndex < images.size() ? images[currentImageIndex] : VK_NULL_HANDLE,
                           vkCtx->getSwapchainExtent(), in);
}

void Renderer::recordPreviewComposites() {
    // Here, outside every pass of the scene, and not as a pre-pass ahead of it.
    //
    // A preview draws 4x multisampled into its own target. As a pre-pass it
    // left its pipeline bound when the scene pass opened, and at character
    // select - no world, so no shadow pass and no draw in the scene pass to
    // bind anything else - nothing replaced it. With the scene at 2x, NVIDIA
    // (591.86, RTX 2070 SUPER) then lost the device inside the empty scene
    // pass on every launch: "invalid write at 0x0", the queue last reaching
    // the scene pass and not its end. Leaving the pipeline unbound, drawing
    // the preview at the scene's 2x, or a scene at 1x or 4x, each kept the
    // device; validation has nothing to say, since a pipeline bound across
    // passes is legal until something draws with it. After the scene, the
    // only pass left to open is the single-sampled overlay - which is also
    // the pass that samples the previews, so they are still drawn first.
    if (activePreviews_.empty() || !vkCtx || currentCmd == VK_NULL_HANDLE) return;
    const uint32_t frame = vkCtx->getCurrentFrame();
    for (auto* preview : activePreviews_) {
        if (preview && preview->isModelLoaded()) preview->compositePass(currentCmd, frame);
    }
    vkCtx->gpuMark(currentCmd, "character previews");
}

void Renderer::recordSunShafts() {
    if (!sunShafts_ || currentCmd == VK_NULL_HANDLE) return;
    SunShafts::FrameInputs in;
    const auto& images = vkCtx->getSwapchainImages();
    if (sunShaftsEnabled_ && worldDrawnThisFrame_ && camera && lightingManager &&
        currentImageIndex < images.size() &&
        !(passAblation_ && passAblation_->skip(AblationPass::SunShafts))) {
        const auto& lp = lightingManager->getLightingParams();
        // The sun the lens flare draws around: the client's sun curve
        // (0x007eecc0), not the light's direction.
        const glm::vec3 sunDir = lp.sunDir;
        const SunOnScreen sun = sunScreenPosition(camera->getViewMatrix(),
                                                  camera->getProjectionMatrix(), sunDir);
        if (sun.inFront) {
            // Screen strength: 1 would be the sky's own colour over anything a
            // fully lit walk crosses. Less, because the sky around the sun is
            // already the brightest thing on screen.
            float strength = 0.8f;
            // Up out of the horizon and gone again as it sets. A sun under the
            // ground lights nothing to stream from.
            strength *= glm::smoothstep(-0.02f, 0.1f, sunDir.z);
            // In view, or streaming in from just past an edge, fading out as
            // it goes half a screen beyond one.
            const glm::vec2 past = glm::max(glm::abs(sun.uv - 0.5f) - 0.5f, glm::vec2(0.0f));
            strength *= 1.0f - glm::smoothstep(0.0f, 0.5f, std::max(past.x, past.y));
            // Rain and snow put a lid over it.
            if (weather) strength *= 1.0f - 0.8f * glm::clamp(weather->getIntensity(), 0.0f, 1.0f);

            // The sun's own colour, kept in hue and not in brightness, and
            // half white: the rays should warm at dusk, not turn orange.
            const glm::vec3 c = lp.diffuseColor;
            const float peak = std::max({c.r, c.g, c.b, 1e-3f});
            in.tint = glm::mix(glm::vec3(1.0f), c / peak, 0.5f);
            in.sunUV = sun.uv;
            in.strength = strength;
        }
    }
    // Called every frame, strength zero included, so the composite knows
    // there is nothing of this frame's to add.
    sunShafts_->record(currentCmd, vkCtx->getCurrentFrame(),
                       currentImageIndex < images.size() ? images[currentImageIndex] : VK_NULL_HANDLE,
                       vkCtx->getSwapchainExtent(), in);
}

// Build the per-frame render graph for off-screen pre-passes.
// Declares passes as graph nodes with input/output dependencies.
// compile() performs topological sort; execute() runs them with auto barriers.
void Renderer::buildFrameGraph(game::GameHandler* gameHandler) {
    (void)gameHandler;
    if (!renderGraph_) return;

    renderGraph_->reset();

    auto shadowDepth = renderGraph_->findResource("shadow_depth");
    auto reflTex = renderGraph_->findResource("reflection_texture");

    // Inside a building the minimap is the indoor one, drawn from the WMO
    // groups' own pictures (0x007f5ba0). Asked here, while the frame is built,
    // rather than in the pass.
    if (minimap && minimap->isEnabled() && camera) {
        glm::vec3 minimapCenter = camera->getPosition();
        if (cameraController && cameraController->isThirdPerson())
            minimapCenter = characterPosition;
        loadWmoAreaRows();
        if (wmoRenderer) wmoRenderer->setAreaRows(wmoAreaRows_.empty() ? nullptr : &wmoAreaRows_);
        minimap->setIndoorScene(wmoRenderer
            ? wmoRenderer->indoorMinimapAt(minimapCenter, minimap->insideViewRadius(), playerLinks_)
            : std::nullopt);
    }

    // Minimap composites (no dependencies - standalone off-screen render target)
    renderGraph_->addPass("minimap_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            if (minimap && minimap->isEnabled() && camera) {
                glm::vec3 minimapCenter = camera->getPosition();
                if (cameraController && cameraController->isThirdPerson())
                    minimapCenter = characterPosition;
                minimap->compositePass(cmd, minimapCenter);
                vkCtx->gpuMark(cmd, "minimap composite");
            }
        });

    // World map composite (standalone)
    renderGraph_->addPass("worldmap_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            if (worldMap) {
                worldMap->compositePass(cmd);
                vkCtx->gpuMark(cmd, "world map composite");
            }
        });

    // The character previews are not a pre-pass: endFrame records them once
    // the scene's passes are closed (recordPreviewComposites says why).

    // The shadow map has no pass here any more: it is drawn into the frame's
    // prologue (planShadowPass), which is submitted ahead of this buffer, so
    // it is complete and readable before anything below reads it. The two
    // passes below still name it as their input, which costs nothing and
    // says what they read.

    // Volumetric fog → reads this frame's shadow map, outputs the fog volume
    // every world shader samples.
    auto fogVolume = renderGraph_->findResource("volumetric_fog");
    renderGraph_->addPass("volumetric_fog", {shadowDepth}, {fogVolume},
        [this](VkCommandBuffer) {
            renderVolumetricFog();
        });
    renderGraph_->setPassEnabled("volumetric_fog", volumetricThisFrame_);

    // Reflection pre-pass → outputs reflection_texture (reads scene, so after shadow)
    renderGraph_->addPass("reflection_pass", {shadowDepth}, {reflTex},
        [this](VkCommandBuffer cmd) {
            renderReflectionPass();
            vkCtx->gpuMark(cmd, "water reflection");
        });

    renderGraph_->compile();
}

} // namespace rendering
} // namespace wowee
