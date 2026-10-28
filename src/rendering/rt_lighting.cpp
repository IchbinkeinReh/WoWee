#include "rendering/rt_lighting.hpp"
#include "rendering/rt_reconstruct.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/env_flag.hpp"
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
    glm::vec4 tune;
    glm::vec4 tune2;
    glm::vec4 tune3;
    glm::vec4 rays;
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
    kSceneGbuf0 = 13,   // the opaque surfaces' G-buffer (rt_gbuffer.glsli)
    kSceneGbuf1 = 14,
    kBindingCount = 15,
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

// The scene depth to the trace and the apply pass, and back to the passes
// after them.
void depthBarrier(VkCommandBuffer cmd, VkImage image, bool toRead) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (toRead) {
        b.srcStageMask = tests;
        b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = readers;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    } else {
        b.srcStageMask = readers;
        b.srcAccessMask = 0;
        b.dstStageMask = tests;
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
    destroyGbuffer();
    for (auto& s : slots_) {
        destroyBuffer(alloc, s.ubo);
        destroyBuffer(alloc, s.lights);
    }
    destroyImage(dev, alloc, neutral_);
    if (depthView_) vkDestroyImageView(dev, depthView_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    for (VkPipeline* p : {&prepare_, &prepareMs_, &trace_, &temporal_, &filter_, &apply_}) {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (applyRenderPass_) vkDestroyRenderPass(dev, applyRenderPass_, nullptr);
    applyRenderPass_ = VK_NULL_HANDLE;
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

    // One layout for the compute passes and the apply pass, which reads the
    // result the last filter pass leaves through the second filter set.
    std::array<VkDescriptorSetLayoutBinding, kBindingCount> b{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 9},
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

// The apply pass: the scene's colour, loaded and stored, at its sample count,
// left in the layout the G-buffer pass left it in and the scene pass resumes
// from (VkContext::createDeferredScenePasses); and the full-screen pipeline
// over it, per sample under MSAA.
bool RtLighting::createApplyPipeline() {
    VkDevice dev = ctx_->getDevice();
    if (apply_) vkDestroyPipeline(dev, apply_, nullptr);
    if (applyRenderPass_) vkDestroyRenderPass(dev, applyRenderPass_, nullptr);
    apply_ = VK_NULL_HANDLE;
    applyRenderPass_ = VK_NULL_HANDLE;
    for (VkFramebuffer fb : applyFramebuffers_.takeAll()) vkDestroyFramebuffer(dev, fb, nullptr);

    const VkSampleCountFlagBits samples = ctx_->getMsaaSamples();
    const bool msaa = samples > VK_SAMPLE_COUNT_1_BIT;
    const VkImageLayout layout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentDescription colour{};
    colour.format = ctx_->getSwapchainFormat();
    colour.samples = samples;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = layout;
    colour.finalLayout = layout;
    const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &colour;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    if (vkCreateRenderPass(dev, &rp, nullptr, &applyRenderPass_) != VK_SUCCESS) {
        applyRenderPass_ = VK_NULL_HANDLE;
        return false;
    }

    VkShaderModule vert, frag;
    if (!vert.loadFromFile(dev, "assets/shaders/postprocess.vert.spv") ||
        !frag.loadFromFile(dev, msaa ? "assets/shaders/rt_apply_ms.frag.spv" : "assets/shaders/rt_apply.frag.spv")) {
        LOG_ERROR("RtLighting: failed to load the apply pass's shaders");
        vert.destroy();
        frag.destroy();
        return false;
    }
    // dst * outMul + outAdd: the second source scales what is there, the
    // first adds to it; the alpha is left as it is.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    apply_ = PipelineBuilder()
                 .setShaders(vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                 .setVertexInput({}, {})
                 .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                 .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                 .setNoDepthTest()
                 .setColorBlendAttachment(blend)
                 .setMultisample(samples)
                 .setSampleShading(msaa)
                 .setLayout(pipelineLayout_)
                 .setRenderPass(applyRenderPass_)
                 .setDynamicStates(viewportAndScissorDynamic())
                 .build(dev, ctx_->getPipelineCache());
    vert.destroy();
    frag.destroy();
    applySamples_ = samples;
    if (!apply_) LOG_ERROR("RtLighting: failed to create the apply pass's pipeline");
    return apply_ != VK_NULL_HANDLE;
}

bool RtLighting::createImages(VkExtent2D e) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    // Transfer source so a tool can read a result back (tools/rt_probe.cpp),
    // destination to be cleared at creation.
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto make = [&](Target& t) {
        t.img = createImage(dev, alloc, e.width, e.height, kFormat, usage);
        return t.img.image != VK_NULL_HANDLE;
    };
    bool ok = make(raw_) && make(tmp_);
    for (auto& s : slots_) {
        ok = ok && make(s.gbuf) && make(s.hist) && make(s.out) && make(s.point);
    }
    traceExtent_ = e;
    ++viewsGeneration_;
    if (!ok) return false;
    // In GENERAL from the start, cleared: the per-frame sets bind them for
    // every shader that could read them, from the first frame on.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        auto init = [&](const Target& t) {
            transitionImageLayout(cmd, t.img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdClearColorImage(cmd, t.img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        };
        for (const Target* t : {&raw_, &tmp_}) init(*t);
        for (auto& sl : slots_) {
            for (const Target* t : {&sl.gbuf, &sl.hist, &sl.out, &sl.point}) init(*t);
        }
    });
    return true;
}

void RtLighting::destroyImages() {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    for (Target* t : {&raw_, &tmp_}) destroyImage(dev, alloc, t->img);
    for (auto& s : slots_) {
        for (Target* t : {&s.gbuf, &s.hist, &s.out, &s.point}) destroyImage(dev, alloc, t->img);
    }
    // The probes' sets bind the slots' G-buffer views.
    probes_.forgetSets();
    traceExtent_ = {};
    ++viewsGeneration_;
}

bool RtLighting::createGbuffer(VkExtent2D extent, VkSampleCountFlagBits samples) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    gbuf0_ = createImage(dev, alloc, extent.width, extent.height, VkContext::kGbufferFormat, usage, samples);
    gbuf1_ = createImage(dev, alloc, extent.width, extent.height, VkContext::kGbufferFormat, usage, samples);
    gbufExtent_ = extent;
    gbufSamples_ = samples;
    ++viewsGeneration_;
    return gbuf0_.image != VK_NULL_HANDLE && gbuf1_.image != VK_NULL_HANDLE;
}

void RtLighting::destroyGbuffer() {
    VkDevice dev = ctx_->getDevice();
    for (VkFramebuffer fb : gbufferFramebuffers_.takeAll()) vkDestroyFramebuffer(dev, fb, nullptr);
    for (VkFramebuffer fb : applyFramebuffers_.takeAll()) vkDestroyFramebuffer(dev, fb, nullptr);
    destroyImage(dev, ctx_->getAllocator(), gbuf0_);
    destroyImage(dev, ctx_->getAllocator(), gbuf1_);
    gbufExtent_ = {};
    ++viewsGeneration_;
}

void RtLighting::warmUp() {
    if (mode_ == Mode::Off || !scene_ || ctx_->getGbufferRenderPass() == VK_NULL_HANDLE) return;
    if (!pipelinesTried_ && !createPipelines()) return;
    if (!pipelinesOk_) return;
    scene_->setActive(true);
    scene_->uploadAllPending();
}

bool RtLighting::ready() const {
    return active() && traceExtent_.width != 0 && gbuf0_.image != VK_NULL_HANDLE && apply_ != VK_NULL_HANDLE &&
           ctx_->getGbufferRenderPass() != VK_NULL_HANDLE && ctx_->getSceneResumeRenderPass() != VK_NULL_HANDLE &&
           applySamples_ == ctx_->getMsaaSamples() && gbufSamples_ == ctx_->getMsaaSamples();
}

bool RtLighting::prepare(VkExtent2D sceneExtent, VkSampleCountFlagBits samples) {
    ++frameCounter_;
    if (mode_ == Mode::Off || ctx_->getGbufferRenderPass() == VK_NULL_HANDLE) {
        if (traceExtent_.width == 0 && gbuf0_.image == VK_NULL_HANDLE && !scene_->isActive() &&
            !probes_.hasResources()) {
            return false;
        }
        // Nothing of the pass, the G-buffer, the probes or the scene's
        // acceleration structures is kept while off; the per-frame sets go
        // back to the neutral image.
        vkDeviceWaitIdle(ctx_->getDevice());
        destroyImages();
        destroyGbuffer();
        probes_.destroyResources();
        scene_->setActive(false);
        resultFrame_ = 0;
        return true;
    }
    if (!pipelinesTried_ && !createPipelines()) return false;
    if (!pipelinesOk_) return false;
    // Switched on: the whole scene in one hitch here, before the first
    // traced frame, rather than seconds of frames each building a share.
    const bool wasActive = scene_->isActive();
    scene_->setActive(true);
    if (!wasActive) scene_->uploadAllPending();

    // The probe volume lives only in Full mode: its atlas is some 47 MB.
    bool changed = false;
    if ((mode_ == Mode::Full) != probes_.hasResources()) {
        vkDeviceWaitIdle(ctx_->getDevice());
        if (mode_ == Mode::Full) {
            // Failing that, Full lights as ShadowsAO; createResources has said why.
            probes_.createResources();
        } else {
            probes_.destroyResources();
        }
        changed = true;
    }

    // The apply pass is built for the scene's sample count.
    if (applySamples_ != ctx_->getMsaaSamples() || !apply_) {
        vkDeviceWaitIdle(ctx_->getDevice());
        if (!createApplyPipeline()) {
            mode_ = Mode::Off;
            return true;
        }
    }

    if (sceneExtent.width != gbufExtent_.width || sceneExtent.height != gbufExtent_.height ||
        samples != gbufSamples_ || gbuf0_.image == VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx_->getDevice());
        destroyGbuffer();
        if (!createGbuffer(sceneExtent, samples)) {
            LOG_ERROR("RtLighting: failed to create the ", sceneExtent.width, "x", sceneExtent.height,
                      " G-buffer");
            destroyGbuffer();
            mode_ = Mode::Off;
            return true;
        }
        changed = true;
    }

    // One texel per 2x2 block of scene pixels, or one per pixel
    // (setFullResolution): rt_prepare picks the block's nearest surface, and
    // every pass after it works in texels of whatever size this is.
    const VkExtent2D want = fullResolution_
        ? VkExtent2D{std::max(1u, sceneExtent.width), std::max(1u, sceneExtent.height)}
        : VkExtent2D{std::max(1u, (sceneExtent.width + 1) / 2), std::max(1u, (sceneExtent.height + 1) / 2)};
    if (want.width == traceExtent_.width && want.height == traceExtent_.height) return changed;

    vkDeviceWaitIdle(ctx_->getDevice());
    destroyImages();
    if (!createImages(want)) {
        LOG_ERROR("RtLighting: failed to create ", want.width, "x", want.height, " images");
        destroyImages();
        mode_ = Mode::Off;
        return true;
    }
    resultFrame_ = 0;
    LOG_INFO("RtLighting: tracing at ", want.width, "x", want.height);
    return true;
}

VkImageView RtLighting::lightViewForSlot(uint32_t slot) const {
    const Slot& s = slots_[slot];
    return s.out.img.imageView ? s.out.img.imageView : neutral_.imageView;
}

VkImageView RtLighting::pointViewForSlot(uint32_t slot) const {
    const Slot& s = slots_[slot];
    return s.point.img.imageView ? s.point.img.imageView : neutral_.imageView;
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

void RtLighting::releaseFramebuffersLater(std::vector<VkFramebuffer> framebuffers) {
    if (framebuffers.empty()) return;
    VkDevice dev = ctx_->getDevice();
    ctx_->deferAfterAllFrameFences([dev, fbs = std::move(framebuffers)]() {
        for (VkFramebuffer fb : fbs) vkDestroyFramebuffer(dev, fb, nullptr);
    });
}

void RtLighting::setSceneTargetsGeneration(uint64_t generation) {
    // A new generation: the scene's targets were destroyed or made again, and
    // the new views and images may have the old ones' handles. Nothing made
    // for the old ones is found again; it goes once no frame can use it.
    if (generation == gbufferFramebuffers_.generation()) return;
    releaseFramebuffersLater(gbufferFramebuffers_.advance(generation));
    releaseFramebuffersLater(applyFramebuffers_.advance(generation));
    if (depthView_) {
        VkDevice dev = ctx_->getDevice();
        VkImageView old = depthView_;
        ctx_->deferAfterAllFrameFences([dev, old]() { vkDestroyImageView(dev, old, nullptr); });
        depthView_ = VK_NULL_HANDLE;
        // The slots' sets bind it; they are written again with the next one.
        ++viewsGeneration_;
    }
    depthImage_ = VK_NULL_HANDLE;
}

VkFramebuffer RtLighting::gbufferFramebuffer(VkImageView colour, VkImageView depth) {
    if (!gbuf0_.image || colour == VK_NULL_HANDLE || depth == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    const auto key = std::make_pair(colour, depth);
    if (const VkFramebuffer* fb = gbufferFramebuffers_.find(key)) return *fb;
    // The 1x scene alternates between the swapchain's images, so more than
    // one is current at a time; a bound on the count all the same.
    if (gbufferFramebuffers_.size() >= 8) releaseFramebuffersLater(gbufferFramebuffers_.takeAll());
    const VkImageView views[4] = {colour, depth, gbuf0_.imageView, gbuf1_.imageView};
    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = ctx_->getGbufferRenderPass();
    fi.attachmentCount = 4;
    fi.pAttachments = views;
    fi.width = gbufExtent_.width;
    fi.height = gbufExtent_.height;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(ctx_->getDevice(), &fi, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    gbufferFramebuffers_.insert(key, fb);
    return fb;
}

VkFramebuffer RtLighting::applyFramebuffer(VkImageView colour) {
    if (const VkFramebuffer* fb = applyFramebuffers_.find(colour)) return *fb;
    if (applyFramebuffers_.size() >= 8) releaseFramebuffersLater(applyFramebuffers_.takeAll());
    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = applyRenderPass_;
    fi.attachmentCount = 1;
    fi.pAttachments = &colour;
    fi.width = gbufExtent_.width;
    fi.height = gbufExtent_.height;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(ctx_->getDevice(), &fi, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    applyFramebuffers_.insert(colour, fb);
    return fb;
}

VkImageView RtLighting::depthViewFor(VkImage image) {
    // Valid only within one generation of the scene's targets: a new depth
    // image can have the old one's handle (setSceneTargetsGeneration).
    if (image == depthImage_ && depthView_) return depthView_;
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
    ++viewsGeneration_;
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
    VkDescriptorImageInfo depth{pointSampler_, depthView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo gbuf0{pointSampler_, gbuf0_.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo gbuf1{pointSampler_, gbuf1_.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    // Filter passes: history -> out, out -> tmp, tmp -> out. The apply pass
    // reads the result through the second set, whose input is out.
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
        img[kSceneGbuf0] = gbuf0;
        img[kSceneGbuf1] = gbuf1;

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
    s.setsGeneration = viewsGeneration_;
}

void RtLighting::record(VkCommandBuffer cmd, const SceneTarget& target, const FrameInputs& in) {
    if (!ready() || target.depthImage == VK_NULL_HANDLE || target.colourView == VK_NULL_HANDLE) return;
    if (target.extent.width != gbufExtent_.width || target.extent.height != gbufExtent_.height) return;
    if (!depthViewFor(target.depthImage)) return;

    const uint32_t slotIndex = ctx_->getCurrentFrame();
    Slot& s = slots_[slotIndex];
    if (s.setsGeneration != viewsGeneration_) writeSlotSets(slotIndex);

    scene_->update(cmd);
    ctx_->gpuMark(cmd, "rt scene update");

    // Last frame's slot holds history only if last frame traced into it.
    const bool havePrev = resultFrame_ != 0 && resultFrame_ + 1 == frameCounter_;

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
    d.sceneExtent = glm::vec4(target.extent.width, target.extent.height, 1.0f / target.extent.width,
                              1.0f / target.extent.height);
    d.misc = glm::vec4(scene_->hasContent() ? 1.0f : 0.0f, havePrev ? 1.0f : 0.0f, 0.0f, 0.0f);
    d.tune = glm::vec4(in.shadowStrength, in.sunShadow, in.occlusion, in.pointLights);
    // w: a point light the ground or a building stops gives nothing, whatever
    // the point shadows' strength (rt_trace's rtHardShadowVis).
    // WOWEE_RT_HARD_STRUCTURE=0: their shadow as soft as any, as before, to
    // compare.
    static const bool hardStructure = core::envFlagEnabled("WOWEE_RT_HARD_STRUCTURE", true);
    d.tune2 = glm::vec4(in.bakedLampShadow, in.pointShadow, static_cast<float>(in.debugView),
                        hardStructure ? 1.0f : 0.0f);
    d.tune3 = glm::vec4(in.interiorBase, in.interiorFullAt, in.maxLight, in.lampWrap);
    d.rays = glm::vec4(clampRtRays(in.rays.sun), clampRtRays(in.rays.occlusion),
                       clampRtRays(in.rays.pointLights), clampRtRays(in.rays.bounce));
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

    // This slot's images were last read by the blended surfaces and the apply
    // pass of the frame two back; this frame overwrites them.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // The probes first: they need nothing of this frame's depth.
    if (probes) probes_.record(cmd, slotIndex, s.ubo.buffer, sizeof(RtPassData), s.gbuf.img.imageView);

    depthBarrier(cmd, target.depthImage, true);

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

    bind(gbufSamples_ > VK_SAMPLE_COUNT_1_BIT ? prepareMs_ : prepare_, 0);
    vkCmdDispatch(cmd, gx, gy, 1);
    computeToCompute();

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

    // For the apply pass and the blended surfaces after it - and the probe
    // atlas, which blended doodads read too.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    ctx_->gpuMark(cmd, "rt trace");

    recordApply(cmd, target, s);
    depthBarrier(cmd, target.depthImage, false);
    ctx_->gpuMark(cmd, "rt apply");

    prevViewProj_ = in.viewProj;
    prevCameraPos_ = in.cameraPos;
    resultFrame_ = frameCounter_;
}

void RtLighting::recordApply(VkCommandBuffer cmd, const SceneTarget& target, Slot& s) {
    VkFramebuffer fb = applyFramebuffer(target.colourView);
    if (!fb) return;
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = applyRenderPass_;
    rp.framebuffer = fb;
    rp.renderArea.extent = target.extent;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{};
    vp.width = static_cast<float>(target.extent.width);
    vp.height = static_cast<float>(target.extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{};
    sc.extent = target.extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, apply_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &s.sets[1], 0,
                            nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

RtLighting::ConsumerData RtLighting::consumerData(bool traced) const {
    ConsumerData c;
    // The atlas keeps its light whether or not this frame traces, and is read
    // with the grid it was last written for.
    if (active() && mode_ == Mode::Full) c.gi = probes_.consumerData();
    if (traced && active()) c.params = glm::vec4(static_cast<float>(mode_), 0.0f, 0.0f, 0.0f);
    return c;
}

} // namespace rendering
} // namespace wowee
