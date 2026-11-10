#include "rendering/model_frame_view.hpp"

#include "core/application.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/imgui_texture.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_utils.hpp"

#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace wowee::rendering {



ModelFrameView::ModelFrameView() = default;

ModelFrameView::~ModelFrameView() { shutdown(); }

bool ModelFrameView::ensureSize(pipeline::AssetManager* assets, int width, int height) {
    assets_ = assets;
    width = std::clamp(width, 8, 1024);
    height = std::clamp(height, 8, 1024);
    if (target_ && width == width_ && height == height_) return true;
    auto* renderer = core::Application::getInstance().getRenderer();
    vkCtx_ = renderer ? renderer->getVkContext() : nullptr;
    if (!vkCtx_ || !renderer->getPerFrameSetLayout()) return false;
    if (target_) {
        ::wowee::rendering::deviceWaitIdle(vkCtx_->getDevice());
        destroyTarget();
    }
    if (!createTarget(width, height)) return false;
    width_ = width;
    height_ = height;
    if (!m2_) {
        m2_ = std::make_unique<M2Renderer>();
        m2_->setRenderPassOverride(target_->getRenderPass(), target_->getSampleCount());
        if (!m2_->initialize(vkCtx_, renderer->getPerFrameSetLayout(), assets)) {
            LOG_ERROR("ModelFrameView: M2 renderer would not initialize");
            m2_.reset();
            return false;
        }
        // Nothing of a UI model is culled: the frame shows all of it.
        m2_->setForceNoCull(true);
        camera_ = std::make_unique<Camera>();
    }
    if (!registered_) {
        renderer->registerModelFrame(this);
        registered_ = true;
    }
    return true;
}

bool ModelFrameView::setModel(const std::string& path, const std::string& iconTexture) {
    if (!m2_ || !assets_) return false;
    // .mdx/.mdl read as .m2, and a bare name as one.
    std::string m2Path = path.empty() ? std::string() : pipeline::modelPathToM2(path);
    if (!m2Path.empty() && m2Path.find('.') == std::string::npos) m2Path += ".m2";
    if (m2Path == modelPath_ && iconTexture == iconTexture_) return instanceId_ != 0 || m2Path.empty();
    if (instanceId_ != 0) {
        ::wowee::rendering::deviceWaitIdle(vkCtx_->getDevice());
        m2_->removeInstance(instanceId_);
        instanceId_ = 0;
    }
    if (modelId_ != 0) {
        m2_->unloadModel(modelId_);
        modelId_ = 0;
    }
    modelPath_ = m2Path;
    iconTexture_ = iconTexture;
    cameras_.clear();
    sequenceDurations_.clear();
    if (m2Path.empty()) return true;
    pipeline::M2Model model;
    if (!pipeline::loadM2WithAllAnimations(*assets_, m2Path, model) || !model.isValid()) {
        LOG_WARNING("ModelFrameView: cannot load ", m2Path);
        return false;
    }
    cameras_ = model.cameras;
    for (const auto& seq : model.sequences) sequenceDurations_.emplace_back(seq.id, seq.duration);
    // ReplaceIconTexture's texture in the model's type-14 slots.
    if (!iconTexture.empty()) {
        for (auto& tex : model.textures) {
            if (tex.type != 14) continue;
            tex.type = 0;
            tex.filename = iconTexture;
        }
    }
    modelId_ = 1;
    if (!m2_->loadModel(model, modelId_)) {
        modelId_ = 0;
        return false;
    }
    instanceId_ = m2_->createInstance(modelId_, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f, false);
    sequence_ = -1;
    sequenceSerial_ = 0;
    return instanceId_ != 0;
}

void ModelFrameView::setFrame(float widthDdc, float heightDdc, const model_frame::Pose& pose,
                              float ddcScreenHeight, int camera, int sequence, int sequenceTimeMs,
                              uint32_t sequenceSerial) {
    if (!m2_ || !camera_ || instanceId_ == 0 || widthDdc <= 0.0f || heightDdc <= 0.0f) return;
    const auto& cams = cameras_;
    if (camera >= 0 && static_cast<size_t>(camera) < cams.size()) {
        // 0x004bf0c0: through the model's own camera, its position and
        // target (0x004c1290 7, 8) and its field of view, on the frame's
        // shape. The model stands at its pose.
        const auto& cam = cams[static_cast<size_t>(camera)];
        const glm::mat4 model = model_frame::modelMatrix(pose, ddcScreenHeight);
        m2_->setInstanceTransform(instanceId_, model);
        const glm::vec3 eye = glm::vec3(model * glm::vec4(cam.positionBase, 1.0f));
        const glm::vec3 target = glm::vec3(model * glm::vec4(cam.targetBase, 1.0f));
        camera_->setPosition(eye);
        camera_->setLookDirection(target - eye, glm::vec3(0.0f, 0.0f, 1.0f));
        const float aspect = widthDdc / heightDdc;
        // The camera's diagonal field of view, made vertical for the frame.
        const float diag = cam.fov > 0.0f && cam.fov < 3.1f ? cam.fov : 0.7f;
        const float vfov = 2.0f * std::atan(std::tan(diag * 0.5f) / std::sqrt(1.0f + aspect * aspect));
        const float nearClip = cam.nearClip > 0.0f ? cam.nearClip : 0.1f;
        const float farClip = cam.farClip > nearClip ? cam.farClip : 1000.0f;
        glm::mat4 proj = glm::perspectiveRH_ZO(vfov, aspect, nearClip, farClip);
        proj[1][1] *= -1.0f;
        camera_->setProjectionOverride(proj);
    } else {
        // 0x004bee60: orthographic over the frame's rect, the model's origin
        // at its bottom-left corner, X right, Y up, z -500..500.
        m2_->setInstanceTransform(instanceId_, model_frame::modelMatrix(pose, ddcScreenHeight));
        const glm::vec3 eye(widthDdc * 0.5f, heightDdc * 0.5f, 1.0f);
        camera_->setPosition(eye);
        camera_->setLookDirection(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj = glm::orthoRH_ZO(-widthDdc * 0.5f, widthDdc * 0.5f, -heightDdc * 0.5f, heightDdc * 0.5f,
                                         -499.0f, 501.0f);
        proj[1][1] *= -1.0f;
        camera_->setProjectionOverride(proj);
    }
    if (sequence != sequence_ || sequenceSerial != sequenceSerial_) {
        sequence_ = sequence;
        sequenceSerial_ = sequenceSerial;
        const uint32_t anim = sequence >= 0 ? static_cast<uint32_t>(sequence) : 0u;
        m2_->setInstanceAnimation(instanceId_, anim, true);
        if (sequenceTimeMs > 0) {
            for (const auto& [id, duration] : sequenceDurations_) {
                if (id != anim || duration == 0) continue;
                m2_->setInstanceAnimationFraction(
                    instanceId_, static_cast<float>(static_cast<uint32_t>(sequenceTimeMs) % duration) /
                                     static_cast<float>(duration));
                break;
            }
        }
    }
}

void ModelFrameView::update(float deltaTime) {
    if (!m2_ || !camera_ || instanceId_ == 0) return;
    m2_->update(deltaTime, camera_->getPosition(), camera_->getViewProjectionMatrix());
}

void ModelFrameView::compositePass(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!compositeRequested_) return;
    compositeRequested_ = false;
    if (!m2_ || !camera_ || !target_ || instanceId_ == 0) return;
    const uint32_t fi = frameIndex % kFrames;
    GPUPerFrameData ubo{};
    ubo.view = camera_->getViewMatrix();
    ubo.projection = camera_->getProjectionMatrix();
    ubo.lightSpaceMatrix = glm::mat4(1.0f);
    // The interface's light: the model frame's default (0x0095f650) is white
    // from the viewer's side.
    if (light_.enabled) {
        // SetLight's: a point light shines from where it is toward the
        // model, a directional one along its vector.
        glm::vec3 dir = light_.omni ? -light_.vec : light_.vec;
        if (glm::dot(dir, dir) < 1e-12f) dir = glm::vec3(0.0f, 0.0f, -1.0f);
        ubo.lightDir = glm::vec4(glm::normalize(dir), 0.0f);
        ubo.lightColor = glm::vec4(light_.diffuse, 0.0f);
        ubo.ambientColor = glm::vec4(light_.ambient, 0.0f);
    } else {
        // No light set: the model's lighting starts at nothing each frame
        // (0x00831af0 -> 0x00834900) and the frame's callback adds its
        // M2Light only while SetLight has it on (0x00960070), so a lit
        // surface is dark and only unlit batches and particles show. The
        // direction is the one 0x00835280 falls back to, straight down.
        ubo.lightDir = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
        ubo.lightColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        ubo.ambientColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    ubo.viewPos = glm::vec4(camera_->getPosition(), 0.0f);
    if (fog_.enabled && fog_.farDist > fog_.nearDist) {
        ubo.fogColor = glm::vec4(fog_.color, 1.0f);
        ubo.fogParams = glm::vec4(fog_.nearDist, fog_.farDist, 0.0f, 0.0f);
    } else {
        ubo.fogColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        ubo.fogParams = glm::vec4(99999.0f, 100000.0f, 0.0f, 0.0f);
    }
    ubo.shadowParams = glm::vec4(0.0f);
    ubo.cascadeInfo = glm::vec4(0.0f);
    std::memcpy(uboMapped_[fi], &ubo, sizeof(GPUPerFrameData));
    // Nothing behind the model: the frame under it shows through.
    target_->beginPass(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
    m2_->prepareRender(fi);
    m2_->render(cmd, perFrameSet_[fi], *camera_);
    m2_->renderM2Particles(cmd, perFrameSet_[fi]);
    target_->endPass(cmd);
    composited_ = true;
}

uint64_t ModelFrameView::textureId() const {
    return composited_ ? reinterpret_cast<uint64_t>(imguiTexture_) : 0u;
}

bool ModelFrameView::createTarget(int width, int height) {
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    target_ = std::make_unique<VkRenderTarget>();
    if (!target_->create(*vkCtx_, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         VK_FORMAT_R8G8B8A8_UNORM, true, VK_SAMPLE_COUNT_4_BIT)) {
        target_.reset();
        return false;
    }
    // Readable before its first draw.
    vkCtx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = target_->getColorImage();
        b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1,
                              .baseArrayLayer = 0, .layerCount = 1};
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
    });
    // A 1x1 shadow map at depth 1: nothing in shadow.
    {
        VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_D16_UNORM;
        ci.extent = {.width = 1, .height = 1, .depth = 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(allocator, &ci, &ai, &dummyShadowImage_, &dummyShadowAlloc_, nullptr) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = dummyShadowImage_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_D16_UNORM;
        vi.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1,
                               .baseArrayLayer = 0, .layerCount = 1};
        if (vkCreateImageView(device, &vi, nullptr, &dummyShadowView_) != VK_SUCCESS) return false;
        vkCtx_->immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 t{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            t.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            t.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            t.image = dummyShadowImage_;
            t.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            t.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            t.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            t.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            t.subresourceRange = vi.subresourceRange;
            t.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            VkDependencyInfo d1{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d1.imageMemoryBarrierCount = 1;
            d1.pImageMemoryBarriers = &t;
            cmdPipelineBarrier2(cmd, d1);
            VkClearDepthStencilValue clear{.depth = 1.0f, .stencil = 0};
            vkCmdClearDepthStencilImage(cmd, dummyShadowImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                        &vi.subresourceRange);
            VkImageMemoryBarrier2 r = t;
            r.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            r.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            r.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            r.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkDependencyInfo d2{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d2.imageMemoryBarrierCount = 1;
            d2.pImageMemoryBarriers = &r;
            cmdPipelineBarrier2(cmd, d2);
        });
    }
    // The per-frame sets, laid out as the scene's are.
    {
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = kFrames;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = kFrames * 5;
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = kFrames;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &ci, nullptr, &descPool_) != VK_SUCCESS) return false;
    }
    auto* renderer = core::Application::getInstance().getRenderer();
    VkDescriptorSetLayout layout = renderer->getPerFrameSetLayout();
    for (uint32_t i = 0; i < kFrames; ++i) {
        VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(GPUPerFrameData);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mi{};
        if (vmaCreateBuffer(allocator, &bi, &ai, &ubo_[i], &uboAlloc_[i], &mi) != VK_SUCCESS) return false;
        uboMapped_[i] = mi.pMappedData;
        VkDescriptorSetAllocateInfo sa{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sa.descriptorPool = descPool_;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(device, &sa, &perFrameSet_[i]) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo buf{.buffer = ubo_[i], .offset = 0, .range = sizeof(GPUPerFrameData)};
        VkDescriptorImageInfo shadow{};
        shadow.imageView = dummyShadowView_;
        shadow.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo fog{};
        fog.imageView = renderer->getNeutralFogVolumeView();
        fog.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo rt{};
        rt.imageView = renderer->getNeutralRtLightingView();
        rt.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w[6]{};
        for (uint32_t b = 0; b < 6; ++b) {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = perFrameSet_[i];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].pBufferInfo = &buf;
        w[1].pImageInfo = &shadow;
        w[2].pImageInfo = &fog;
        for (uint32_t b = 3; b < 6; ++b) w[b].pImageInfo = &rt;
        vkUpdateDescriptorSets(device, 6, w, 0, nullptr);
    }
    imguiTexture_ = ImGui_ImplVulkan_AddTexture(target_->getSampler(), target_->getColorImageView(),
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    composited_ = false;
    return true;
}

void ModelFrameView::destroyTarget() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    if (imguiTexture_) removeImGuiTexture(imguiTexture_);
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (ubo_[i]) vmaDestroyBuffer(allocator, ubo_[i], uboAlloc_[i]);
        ubo_[i] = VK_NULL_HANDLE;
        uboAlloc_[i] = VK_NULL_HANDLE;
        perFrameSet_[i] = VK_NULL_HANDLE;
    }
    if (descPool_) vkDestroyDescriptorPool(device, descPool_, nullptr);
    descPool_ = VK_NULL_HANDLE;
    if (dummyShadowView_) vkDestroyImageView(device, dummyShadowView_, nullptr);
    dummyShadowView_ = VK_NULL_HANDLE;
    if (dummyShadowImage_) vmaDestroyImage(allocator, dummyShadowImage_, dummyShadowAlloc_);
    dummyShadowImage_ = VK_NULL_HANDLE;
    if (target_) target_->destroy(device, allocator);
    target_.reset();
    composited_ = false;
}

void ModelFrameView::shutdown() {
    if (registered_) {
        if (auto* renderer = core::Application::getInstance().getRenderer()) renderer->unregisterModelFrame(this);
        registered_ = false;
    }
    if (vkCtx_ && (m2_ || target_)) ::wowee::rendering::deviceWaitIdle(vkCtx_->getDevice());
    if (m2_) {
        m2_->shutdown();
        m2_.reset();
    }
    camera_.reset();
    cameras_.clear();
    destroyTarget();
    instanceId_ = 0;
    modelId_ = 0;
    modelPath_.clear();
}

}  // namespace wowee::rendering
