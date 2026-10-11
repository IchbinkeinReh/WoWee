#include "rendering/rt_lighting.hpp"
#include "rendering/rt_reconstruct.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

// Must match RtPassData in rt_common.glsli.
struct RtPassData {
    glm::mat4 invViewProj;
    glm::mat4 prevViewProj;
    glm::vec4 cameraPos;
    glm::vec4 prevCameraPos;
    glm::vec4 sunDir;
    glm::vec4 sunColor;
    glm::vec4 skyColor;
    glm::vec4 params;
    glm::vec4 extent;
    glm::vec4 sceneExtent;
    glm::vec4 misc;
    glm::mat4 invViewProjRel;
    glm::vec4 unitMap;
    RtGiProbes::PassData gi;
};

enum Binding : uint32_t {
    kUbo = 0,
    kSceneDepth = 1,
    kGbufStore = 2,
    kGbufCur = 3,
    kRawStore = 4,
    kGbufPrev = 5,
    kRaw = 6,
    kHistPrev = 7,
    kHistStore = 8,
    kFilterIn = 9,
    kFilterOut = 10,
    kPointStore = 11,   // the slot's point lights, unshadowed (rt_trace.comp)
    kPointLights = 12,  // the slot's light list
    kBindingCount = 13,
};

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr float kAoRadius = 3.0f;         // yards
constexpr float kShadowRayLength = 1500.0f;
// The sun's angular radius is about a quarter of a degree. Twice that gives a
// penumbra that reads at game distances without looking like an area light.
constexpr float kSunTanRadius = 0.0087f;

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = src;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dst;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    cmdPipelineBarrier2(cmd, dep);
}

void depthBarrier(VkCommandBuffer cmd, VkImage image, bool toRead) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    if (toRead) {
        b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    } else {
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = 0;
        b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

} // namespace

RtLighting::~RtLighting() { shutdown(); }

bool RtLighting::initialize(VkContext* ctx, RtScene* scene) {
    ctx_ = ctx;
    scene_ = scene;
    probes_.initialize(ctx, scene);
    VkDevice dev = ctx_->getDevice();

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &linearSampler_) != VK_SUCCESS) return false;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev, &si, nullptr, &pointSampler_) != VK_SUCCESS) return false;

    // What the per-frame sets bind while there is no result. Its contents are
    // never read - the block's switch is off - but the binding must be valid.
    neutral_ = createImage(dev, ctx_->getAllocator(), 1, 1, kFormat,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!neutral_.image) return false;
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImageLayout(cmd, neutral_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue c{};
        c.float32[0] = c.float32[1] = 1.0f;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, neutral_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        transitionImageLayout(cmd, neutral_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_GENERAL);
    });
    return true;
}

void RtLighting::shutdown() {
    if (!ctx_) return;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    vkDeviceWaitIdle(dev);
    probes_.shutdown();
    destroyImages();
    for (auto& s : slots_) {
        destroyBuffer(alloc, s.ubo);
        destroyBuffer(alloc, s.lights);
    }
    destroyImage(dev, alloc, neutral_);
    if (depthView_) vkDestroyImageView(dev, depthView_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    for (VkPipeline* p : {&prepare_, &prepareMs_, &trace_, &temporal_, &filter_}) {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (pipelineLayout_) vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (linearSampler_) vkDestroySampler(dev, linearSampler_, nullptr);
    if (pointSampler_) vkDestroySampler(dev, pointSampler_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    linearSampler_ = pointSampler_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

bool RtLighting::createPipelines() {
    pipelinesTried_ = true;
    VkDevice dev = ctx_->getDevice();

    std::array<VkDescriptorSetLayoutBinding, kBindingCount> b{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    b[kUbo].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (uint32_t i : {kGbufStore, kRawStore, kHistStore, kFilterOut, kPointStore}) {
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    b[kPointLights].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = kBindingCount;
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &setLayout_) != VK_SUCCESS) return false;

    constexpr uint32_t kSets = MAX_FRAMES_IN_FLIGHT * 3;
    VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 6},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets * 5},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets},
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kSets;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &pool_) != VK_SUCCESS) return false;
    for (auto& s : slots_) {
        VkDescriptorSetLayout layouts[3] = {setLayout_, setLayout_, setLayout_};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 3;
        ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(dev, &ai, s.sets) != VK_SUCCESS) return false;
        s.ubo = createBuffer(ctx_->getAllocator(), sizeof(RtPassData),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (!s.ubo.buffer) return false;
        s.lights = createBuffer(ctx_->getAllocator(), sizeof(rt_lights::PointLightGPU) * kMaxPointLights,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (!s.lights.buffer) return false;
    }

    VkDescriptorSetLayout setLayouts[2] = {setLayout_, scene_->descriptorSetLayout()};
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 2;
    pl.pSetLayouts = setLayouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    auto make = [&](const char* path, VkPipeline& out) {
        VkShaderModule module;
        if (!module.loadFromFile(dev, path)) {
            LOG_ERROR("RtLighting: failed to load ", path);
            return false;
        }
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        ci.layout = pipelineLayout_;
        const bool ok = vkCreateComputePipelines(dev, ctx_->getPipelineCache(), 1, &ci, nullptr,
                                                 &out) == VK_SUCCESS;
        module.destroy();
        if (!ok) LOG_ERROR("RtLighting: failed to create the pipeline for ", path);
        return ok;
    };
    const bool hw = scene_->backend() == RtScene::Backend::Hardware;
    pipelinesOk_ = make("assets/shaders/rt_prepare.comp.spv", prepare_) &&
                   make("assets/shaders/rt_prepare_ms.comp.spv", prepareMs_) &&
                   make(hw ? "assets/shaders/rt_trace_hw.comp.spv"
                           : "assets/shaders/rt_trace.comp.spv", trace_) &&
                   make("assets/shaders/rt_temporal.comp.spv", temporal_) &&
                   make("assets/shaders/rt_filter.comp.spv", filter_);
    if (!pipelinesOk_) LOG_ERROR("RtLighting: pipelines unavailable, ray traced lighting stays off");
    return pipelinesOk_;
}

bool RtLighting::createImages(VkExtent2D e) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    // Transfer source so a tool can read a result back (tools/rt_probe.cpp).
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto make = [&](Target& t) {
        t.img = createImage(dev, alloc, e.width, e.height, kFormat, usage);
        return t.img.image != VK_NULL_HANDLE;
    };
    bool ok = make(raw_) && make(tmp_);
    for (auto& s : slots_) {
        ok = ok && make(s.gbuf) && make(s.hist) && make(s.out) && make(s.point);
        s.wroteImages = false;
    }
    traceExtent_ = e;
    imagesNeedInit_ = true;
    return ok;
}

void RtLighting::destroyImages() {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    for (Target* t : {&raw_, &tmp_}) destroyImage(dev, alloc, t->img);
    for (auto& s : slots_) {
        for (Target* t : {&s.gbuf, &s.hist, &s.out, &s.point}) destroyImage(dev, alloc, t->img);
        s.wroteImages = false;
        s.boundDepth = VK_NULL_HANDLE;
    }
    traceExtent_ = {};
}

bool RtLighting::prepare(VkExtent2D sceneExtent) {
    ++frameCounter_;
    if (mode_ == Mode::Off) {
        if (traceExtent_.width == 0 && !scene_->isActive() && !probes_.hasResources()) return false;
        // Nothing of the pass, the probes or the scene's acceleration
        // structures is kept while off; the per-frame sets go back to the
        // neutral image.
        vkDeviceWaitIdle(ctx_->getDevice());
        destroyImages();
        probes_.destroyResources();
        scene_->setActive(false);
        havePrev_ = false;
        return true;
    }
    if (!pipelinesTried_ && !createPipelines()) return false;
    if (!pipelinesOk_) return false;
    scene_->setActive(true);

    // The probe volume lives only in Full mode: its atlas is some 47 MB.
    bool probesChanged = false;
    if ((mode_ == Mode::Full) != probes_.hasResources()) {
        vkDeviceWaitIdle(ctx_->getDevice());
        if (mode_ == Mode::Full) {
            // Failing that, Full lights as ShadowsAO; createResources has said why.
            probes_.createResources();
        } else {
            probes_.destroyResources();
        }
        probesChanged = true;
    }

    const VkExtent2D want{std::max(1u, (sceneExtent.width + 1) / 2),
                          std::max(1u, (sceneExtent.height + 1) / 2)};
    if (want.width == traceExtent_.width && want.height == traceExtent_.height) return probesChanged;

    vkDeviceWaitIdle(ctx_->getDevice());
    destroyImages();
    if (!createImages(want)) {
        LOG_ERROR("RtLighting: failed to create ", want.width, "x", want.height, " images");
        destroyImages();
        mode_ = Mode::Off;
        return true;
    }
    sceneExtent_ = sceneExtent;
    havePrev_ = false;
    resultFrame_ = 0;
    LOG_INFO("RtLighting: tracing at ", want.width, "x", want.height);
    return true;
}

VkImageView RtLighting::lightViewForSlot(uint32_t slot) const {
    const Slot& other = slots_[(slot + 1) % MAX_FRAMES_IN_FLIGHT];
    return other.out.img.imageView ? other.out.img.imageView : neutral_.imageView;
}

VkImageView RtLighting::pointViewForSlot(uint32_t slot) const {
    const Slot& other = slots_[(slot + 1) % MAX_FRAMES_IN_FLIGHT];
    return other.point.img.imageView ? other.point.img.imageView : neutral_.imageView;
}

void RtLighting::setPointLights(const std::vector<rt_lights::PointLight>& lights) {
    pointLights_.clear();
    const size_t n = std::min<size_t>(lights.size(), kMaxPointLights);
    for (size_t i = 0; i < n; ++i) pointLights_.push_back(rt_lights::toGpu(lights[i]));
}

VkImageView RtLighting::giViewForSlot(uint32_t) const {
    // One atlas for both slots: each frame updates only a share of it, so
    // double-buffering it would mean copying the rest every frame. The update
    // runs after the opaque surfaces that read it, behind a barrier.
    return probes_.hasResources() ? probes_.atlasView() : neutral_.imageView;
}

VkImageView RtLighting::depthViewFor(VkImage image, bool msaa) {
    if (image == depthImage_ && msaa == depthViewMsaa_ && depthView_) return depthView_;
    VkDevice dev = ctx_->getDevice();
    if (depthView_) {
        VkImageView old = depthView_;
        ctx_->deferAfterAllFrameFences([dev, old]() { vkDestroyImageView(dev, old, nullptr); });
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ctx_->getDepthFormat();
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    depthView_ = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vi, nullptr, &depthView_) != VK_SUCCESS) depthView_ = VK_NULL_HANDLE;
    depthImage_ = image;
    depthViewMsaa_ = msaa;
    for (auto& s : slots_) s.boundDepth = VK_NULL_HANDLE;
    return depthView_;
}

void RtLighting::writeSlotSets(uint32_t slotIndex) {
    Slot& s = slots_[slotIndex];
    Slot& prev = slots_[(slotIndex + 1) % MAX_FRAMES_IN_FLIGHT];

    VkDescriptorBufferInfo ubo{s.ubo.buffer, 0, sizeof(RtPassData)};
    VkDescriptorBufferInfo lights{s.lights.buffer, 0, VK_WHOLE_SIZE};
    auto sampled = [&](const Target& t) {
        return VkDescriptorImageInfo{pointSampler_, t.img.imageView, VK_IMAGE_LAYOUT_GENERAL};
    };
    auto storage = [&](const Target& t) {
        return VkDescriptorImageInfo{VK_NULL_HANDLE, t.img.imageView, VK_IMAGE_LAYOUT_GENERAL};
    };
    VkDescriptorImageInfo depth{pointSampler_, depthView_,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

    // Filter passes: history -> out, out -> tmp, tmp -> out.
    const Target* filterIn[3] = {&s.hist, &s.out, &tmp_};
    const Target* filterOut[3] = {&s.out, &tmp_, &s.out};

    for (int pass = 0; pass < 3; ++pass) {
        std::array<VkDescriptorImageInfo, kBindingCount> img{};
        img[kSceneDepth] = depth;
        img[kGbufStore] = storage(s.gbuf);
        img[kGbufCur] = sampled(s.gbuf);
        img[kRawStore] = storage(raw_);
        img[kGbufPrev] = sampled(prev.gbuf);
        img[kRaw] = sampled(raw_);
        img[kHistPrev] = sampled(prev.hist);
        img[kHistStore] = storage(s.hist);
        img[kFilterIn] = sampled(*filterIn[pass]);
        img[kFilterOut] = storage(*filterOut[pass]);
        img[kPointStore] = storage(s.point);

        std::array<VkWriteDescriptorSet, kBindingCount> w{};
        for (uint32_t i = 0; i < kBindingCount; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s.sets[pass];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            if (i == kUbo) {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w[i].pBufferInfo = &ubo;
            } else if (i == kPointLights) {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &lights;
            } else {
                const bool isStorage = img[i].sampler == VK_NULL_HANDLE;
                w[i].descriptorType = isStorage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &img[i];
            }
        }
        vkUpdateDescriptorSets(ctx_->getDevice(), kBindingCount, w.data(), 0, nullptr);
    }
    s.wroteImages = true;
    s.boundDepth = depthImage_;
}

void RtLighting::record(VkCommandBuffer cmd, VkImage sceneDepth, VkExtent2D sceneExtent,
                        bool depthMsaa, const FrameInputs& in) {
    if (!active() || traceExtent_.width == 0 || sceneDepth == VK_NULL_HANDLE) return;
    if (sceneExtent.width != sceneExtent_.width || sceneExtent.height != sceneExtent_.height) {
        // The scene changed size under us; prepare() resizes next frame.
        sceneExtent_ = sceneExtent;
        traceExtent_ = {};
        return;
    }
    if (!depthViewFor(sceneDepth, depthMsaa)) return;

    const uint32_t slotIndex = ctx_->getCurrentFrame();
    Slot& s = slots_[slotIndex];
    if (!s.wroteImages || s.boundDepth != depthImage_) writeSlotSets(slotIndex);

    scene_->update(cmd);
    ctx_->gpuMark(cmd, "rt scene update");

    if (imagesNeedInit_) {
        imagesNeedInit_ = false;
        std::vector<VkImageMemoryBarrier2> barriers;
        auto add = [&](const Target& t) {
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t.img.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers.push_back(b);
        };
        for (const Target* t : {&raw_, &tmp_}) add(*t);
        for (auto& sl : slots_) {
            for (const Target* t : {&sl.gbuf, &sl.hist, &sl.out, &sl.point}) add(*t);
        }
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        cmdPipelineBarrier2(cmd, dep);
    }

    // Last frame's slot holds history only if last frame traced into it.
    const bool havePrev = havePrev_ && resultFrame_ != 0 && resultFrame_ + 1 == frameCounter_;

    RtPassData d{};
    d.invViewProj = glm::inverse(in.viewProj);
    // NDC -> world less the camera (rt_reconstruct.hpp): the normals are
    // made from differences the world's coordinates would round away.
    d.invViewProjRel = rt_reconstruct::cameraRelativeInverse(in.viewProj, in.cameraPos);
    d.unitMap = glm::vec4(in.unitMapCenter, std::max(in.unitMapRadius, 0.0f));
    d.prevViewProj = havePrev ? prevViewProj_ : in.viewProj;
    d.cameraPos = glm::vec4(in.cameraPos, static_cast<float>(frameCounter_ & 0xFFFFFF));
    d.prevCameraPos = glm::vec4(havePrev ? prevCameraPos_ : in.cameraPos, 0.0f);
    d.sunDir = glm::vec4(glm::normalize(in.sunDir), kSunTanRadius);
    d.sunColor = glm::vec4(in.sunColor, in.sunUp ? 1.0f : 0.0f);
    d.skyColor = glm::vec4(in.skyColor, 0.0f);
    // The frame's point lights, into this slot's list; z of the block says
    // how many there are.
    const uint32_t lightCount = static_cast<uint32_t>(pointLights_.size());
    if (lightCount > 0) {
        std::memcpy(s.lights.info.pMappedData, pointLights_.data(),
                    lightCount * sizeof(rt_lights::PointLightGPU));
    }
    d.params = glm::vec4(static_cast<float>(mode_), kAoRadius, static_cast<float>(lightCount),
                         kShadowRayLength);
    d.extent = glm::vec4(traceExtent_.width, traceExtent_.height, 1.0f / traceExtent_.width,
                         1.0f / traceExtent_.height);
    d.sceneExtent = glm::vec4(sceneExtent.width, sceneExtent.height, 1.0f / sceneExtent.width,
                              1.0f / sceneExtent.height);
    d.misc = glm::vec4(scene_->hasContent() ? 1.0f : 0.0f, havePrev ? 1.0f : 0.0f, 0.0f, 0.0f);
    const bool probes = mode_ == Mode::Full && probes_.hasResources();
    if (probes) {
        // The open field the zone's flat ambient was set for: that ambient as
        // the sky, and below the horizon ground of a typical colour lit by it
        // and the sun (giOpenReference in rt_gi.glsli).
        const float albedo = probes_.config().groundAlbedo;
        const glm::vec3 sunOnGround =
            in.sunUp ? in.sunColor * std::max(glm::normalize(in.sunDir).z, 0.0f) : glm::vec3(0.0f);
        const glm::vec3 ground = albedo * (sunOnGround + in.skyColor);
        probes_.beginFrame(slotIndex, in.cameraPos, frameCounter_, in.skyColor, ground, d.gi);
    }
    std::memcpy(s.ubo.info.pMappedData, &d, sizeof(d));

    // The outputs of this slot were last read by the surfaces of the frame
    // after the one that wrote them; this frame overwrites them.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // The probes first: they need nothing of this frame's depth.
    if (probes) probes_.record(cmd, slotIndex, s.ubo.buffer, sizeof(RtPassData), s.gbuf.img.imageView);

    depthBarrier(cmd, sceneDepth, true);

    const uint32_t gx = (traceExtent_.width + 7) / 8;
    const uint32_t gy = (traceExtent_.height + 7) / 8;
    VkDescriptorSet sceneSet = scene_->descriptorSet();
    auto bind = [&](VkPipeline p, int filterPass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        VkDescriptorSet sets[2] = {s.sets[filterPass], sceneSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, sets, 0,
                                nullptr);
    };
    auto computeToCompute = [&]() {
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };

    bind(depthMsaa ? prepareMs_ : prepare_, 0);
    vkCmdDispatch(cmd, gx, gy, 1);
    computeToCompute();
    depthBarrier(cmd, sceneDepth, false);

    // The view's lift for the probe light, from the G-buffer just built and
    // the volume just updated; nothing below reads what it writes.
    if (probes) probes_.recordExposure(cmd, slotIndex);

    bind(trace_, 0);
    vkCmdDispatch(cmd, gx, gy, 1);
    computeToCompute();

    bind(temporal_, 0);
    vkCmdDispatch(cmd, gx, gy, 1);

    const int32_t steps[3] = {1, 2, 4};
    for (int pass = 0; pass < 3; ++pass) {
        computeToCompute();
        bind(filter_, pass);
        const int32_t push[2] = {steps[pass], pass == 2 ? 1 : 0};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, gx, gy, 1);
    }

    // For the next frame's surfaces - and the probe atlas also for what of
    // this frame draws after here: blended doodads read it too.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    prevViewProj_ = in.viewProj;
    prevCameraPos_ = in.cameraPos;
    havePrev_ = true;
    resultFrame_ = frameCounter_;
    resultViewProj_ = in.viewProj;
    resultCameraPos_ = in.cameraPos;
    resultMode_ = mode_;
}

RtLighting::ConsumerData RtLighting::consumerData() const {
    ConsumerData c;
    // The atlas keeps its light whether or not last frame traced, and is read
    // with the grid it was last written for.
    if (active() && mode_ == Mode::Full) c.gi = probes_.consumerData();
    if (!active() || resultFrame_ == 0 || resultFrame_ + 1 != frameCounter_) return c;
    c.viewProj = resultViewProj_;
    c.cameraPos = glm::vec4(resultCameraPos_, 1.0f);
    c.params = glm::vec4(static_cast<float>(resultMode_), 1.0f, 0.0f, 0.0f);
    return c;
}

} // namespace rendering
} // namespace wowee
