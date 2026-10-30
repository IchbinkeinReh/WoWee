#include "rendering/rt_gi_probes.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

enum Binding : uint32_t {
    kUbo = 0,
    kAtlasSampled = 1,
    kAtlasStore = 2,
    kList = 3,
    kRays = 4,
    kGbuf = 5,
    kBindingCount = 6,
};

constexpr VkFormat kAtlasFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// The far cascade's cells are this many of the near one's.
constexpr float kFarScale = 4.0f;
// A ray goes this many cells before it counts as escaping. Long enough that a
// probe in a courtyard sees the walls round it and a probe on a hillside the
// hill across the valley; the far cascade's reach is most of a zone's view.
constexpr float kRayCells = 25.0f;

float envFloat(const char* name, float fallback, float lo, float hi) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return std::clamp(static_cast<float>(std::atof(v)), lo, hi);
}

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

// PCG, for the frame's turn of the ray set.
uint32_t hash32(uint32_t v) {
    uint32_t state = v * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A uniformly random rotation (Shoemake's quaternion from three uniforms).
glm::mat4 randomRotation(uint64_t frame) {
    uint32_t h = hash32(static_cast<uint32_t>(frame) ^ 0x9e3779b9u);
    auto next = [&h]() {
        h = hash32(h);
        return static_cast<float>(h) * (1.0f / 4294967296.0f);
    };
    const float u1 = next(), u2 = next(), u3 = next();
    constexpr float kTwoPi = 6.28318530718f;
    const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
    const glm::quat q(b * std::cos(kTwoPi * u3), a * std::sin(kTwoPi * u2),
                      a * std::cos(kTwoPi * u2), b * std::sin(kTwoPi * u3));
    return glm::mat4_cast(glm::normalize(q));
}

float cascadeSpacing(const RtGiProbes::Config& c, int cascade) {
    return cascade == 0 ? c.spacing : c.spacing * kFarScale;
}

} // namespace

RtGiProbes::~RtGiProbes() { shutdown(); }

void RtGiProbes::initialize(VkContext* ctx, RtScene* scene) {
    ctx_ = ctx;
    scene_ = scene;
    // For comparison runs: every knob that decides how the light looks or
    // what it costs, without a rebuild.
    config_.spacing = envFloat("WOWEE_GI_SPACING", config_.spacing, 1.0f, 16.0f);
    config_.period = static_cast<uint32_t>(envFloat("WOWEE_GI_UPDATE_PERIOD",
                                                    static_cast<float>(config_.period), 1.0f, 64.0f));
    config_.hysteresis = envFloat("WOWEE_GI_HYSTERESIS", config_.hysteresis, 0.0f, 0.995f);
    config_.strength = envFloat("WOWEE_GI_STRENGTH", config_.strength, 0.0f, 4.0f);
    config_.maxRatio = envFloat("WOWEE_GI_MAX_RATIO", config_.maxRatio, 1.0f, 8.0f);
    config_.groundAlbedo = envFloat("WOWEE_GI_GROUND_ALBEDO", config_.groundAlbedo, 0.0f, 1.0f);
    config_.chroma = envFloat("WOWEE_GI_CHROMA", config_.chroma, 0.0f, 1.0f);
    config_.normalize = envFloat("WOWEE_GI_NORMALIZE", config_.normalize, 1.0f, 4.0f);
    config_.debug = static_cast<int>(envFloat("WOWEE_GI_DEBUG", 0.0f, 0.0f, 13.0f));
    LOG_INFO("RtGiProbes: spacing ", config_.spacing, " yd, period ", config_.period,
             ", hysteresis ", config_.hysteresis, ", strength ", config_.strength,
             ", max ratio ", config_.maxRatio, ", ground albedo ", config_.groundAlbedo,
             ", chroma ", config_.chroma, ", normalize ", config_.normalize,
             ", debug ", config_.debug);
}

void RtGiProbes::shutdown() {
    if (!ctx_) return;
    VkDevice dev = ctx_->getDevice();
    vkDeviceWaitIdle(dev);
    destroyResources();
    for (VkPipeline* p : {&trace_, &update_, &exposure_}) {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (pipelineLayout_) vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    pipelinesTried_ = pipelinesOk_ = false;
    ctx_ = nullptr;
}

uint64_t RtGiProbes::gpuBytes(VmaAllocator allocator) const {
    uint64_t n = allocationBytes(allocator, atlas_.allocation) + allocationBytes(allocator, rays_.allocation);
    for (const auto& l : lists_) n += allocationBytes(allocator, l.allocation);
    return n;
}

bool RtGiProbes::createPipelines() {
    pipelinesTried_ = true;
    VkDevice dev = ctx_->getDevice();

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

    std::array<VkDescriptorSetLayoutBinding, kBindingCount> b{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    b[kUbo].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[kAtlasSampled].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[kAtlasStore].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[kList].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[kRays].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[kGbuf].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = kBindingCount;
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &setLayout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT * 2},
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &pool_) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (auto& l : layouts) l = setLayout_;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev, &ai, sets_) != VK_SUCCESS) return false;

    VkDescriptorSetLayout setLayouts[2] = {setLayout_, scene_->descriptorSetLayout()};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 2;
    pl.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    auto make = [&](const char* path, VkPipeline& out) {
        VkShaderModule module;
        if (!module.loadFromFile(dev, path)) {
            LOG_ERROR("RtGiProbes: failed to load ", path);
            return false;
        }
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        ci.layout = pipelineLayout_;
        const bool ok = vkCreateComputePipelines(dev, ctx_->getPipelineCache(), 1, &ci, nullptr,
                                                 &out) == VK_SUCCESS;
        module.destroy();
        if (!ok) LOG_ERROR("RtGiProbes: failed to create the pipeline for ", path);
        return ok;
    };
    const bool hw = scene_->backend() == RtScene::Backend::Hardware;
    pipelinesOk_ = make(hw ? "assets/shaders/rt_gi_trace_hw.comp.spv"
                           : "assets/shaders/rt_gi_trace.comp.spv", trace_) &&
                   make("assets/shaders/rt_gi_update.comp.spv", update_) &&
                   make("assets/shaders/rt_gi_exposure.comp.spv", exposure_);
    if (!pipelinesOk_) LOG_ERROR("RtGiProbes: pipelines unavailable, no probe light");
    return pipelinesOk_;
}

bool RtGiProbes::createResources() {
    if (!ctx_) return false;
    if (hasResources()) return true;
    if (!pipelinesTried_) createPipelines();
    if (!pipelinesOk_) return false;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();

    // Transfer source so a tool can read the volume back (tools/rt_probe.cpp).
    atlas_ = createImage(dev, alloc, gi::kAtlasWidth, gi::kAtlasHeight, kAtlasFormat,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    rays_ = createBuffer(alloc,
                         VkDeviceSize(gi::kTotalProbes) * gi::kRaysPerProbe * 2 * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
    bool ok = atlas_.image && rays_.buffer;
    for (auto& l : lists_) {
        l = createBuffer(alloc, VkDeviceSize(gi::kTotalProbes) * sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        ok = ok && l.buffer && l.info.pMappedData;
    }
    if (!ok) {
        LOG_ERROR("RtGiProbes: failed to create the probe atlas and buffers");
        destroyResources();
        return false;
    }

    // Zero, so nothing read before a probe's first update is anything but
    // dark - though nothing is: the first frame updates every probe, and the
    // surfaces wait for it (consumerData).
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImageLayout(cmd, atlas_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue c{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        transitionImageLayout(cmd, atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_GENERAL);
    });
    haveGrid_ = false;
    frameBegun_ = false;
    for (bool& w : setWritten_) w = false;
    LOG_INFO("RtGiProbes: ", gi::kTotalProbes, " probes in ", gi::kCascades, " cascades, atlas ",
             gi::kAtlasWidth, "x", gi::kAtlasHeight);
    return true;
}

void RtGiProbes::destroyResources() {
    if (!ctx_) return;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    destroyImage(dev, alloc, atlas_);
    destroyBuffer(alloc, rays_);
    for (auto& l : lists_) destroyBuffer(alloc, l);
    for (bool& w : setWritten_) w = false;
    for (VkImageView& v : setGbuf_) v = VK_NULL_HANDLE;
    haveGrid_ = false;
    frameBegun_ = false;
}

void RtGiProbes::writeSet(uint32_t slot, VkBuffer ubo, VkDeviceSize uboSize, VkImageView gbuf) {
    VkDescriptorBufferInfo uboInfo{ubo, 0, uboSize};
    VkDescriptorImageInfo sampled{sampler_, atlas_.imageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo storage{VK_NULL_HANDLE, atlas_.imageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo list{lists_[slot].buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo rays{rays_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo gbufInfo{sampler_, gbuf, VK_IMAGE_LAYOUT_GENERAL};

    std::array<VkWriteDescriptorSet, kBindingCount> w{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = sets_[slot];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[kUbo].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[kUbo].pBufferInfo = &uboInfo;
    w[kAtlasSampled].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[kAtlasSampled].pImageInfo = &sampled;
    w[kAtlasStore].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[kAtlasStore].pImageInfo = &storage;
    w[kList].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[kList].pBufferInfo = &list;
    w[kRays].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[kRays].pBufferInfo = &rays;
    w[kGbuf].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[kGbuf].pImageInfo = &gbufInfo;
    vkUpdateDescriptorSets(ctx_->getDevice(), kBindingCount, w.data(), 0, nullptr);
    setWritten_[slot] = true;
    setGbuf_[slot] = gbuf;
}

void RtGiProbes::beginFrame(uint32_t slot, const glm::vec3& camera, uint64_t frame,
                            const glm::vec3& sky, const glm::vec3& ground, PassData& out) {
    out = PassData{};
    frameBegun_ = false;
    if (!hasResources()) return;

    list_.clear();
    for (int c = 0; c < gi::kCascades; ++c) {
        const float spacing = cascadeSpacing(config_, c);
        nextGridMin_[c] = gi::scrolledGridMin(camera, spacing, gridMin_[c], haveGrid_);
        // The far cascade changes slowly and is seen from far: half as often.
        const uint32_t period = config_.period * (c == 0 ? 1u : 2u);
        gi::appendUpdateList(list_, c, nextGridMin_[c], gridMin_[c], haveGrid_, period, frame);

        out.volume[c] = glm::vec4(glm::vec3(nextGridMin_[c]), spacing);
        out.volumeMod[c] = glm::vec4(glm::vec3(gi::storageOf(nextGridMin_[c])), 0.0f);
        if (haveGrid_) {
            out.prevVolume[c] = glm::vec4(glm::vec3(gridMin_[c]), spacing);
            out.prevVolumeMod[c] = glm::vec4(glm::vec3(gi::storageOf(gridMin_[c])), 0.0f);
        }
    }
    listCount_ = static_cast<uint32_t>(list_.size());
    std::memcpy(lists_[slot].info.pMappedData, list_.data(), list_.size() * sizeof(uint32_t));

    out.rotation = randomRotation(frame);
    out.sky = glm::vec4(sky, config_.hysteresis);
    out.ground = glm::vec4(ground, 0.0f);
    out.rayLength = glm::vec4(cascadeSpacing(config_, 0) * kRayCells,
                              cascadeSpacing(config_, 1) * kRayCells, config_.normalize, 0.0f);
    nextSky_ = sky;
    nextGround_ = ground;
    frameBegun_ = true;
}

void RtGiProbes::record(VkCommandBuffer cmd, uint32_t slot, VkBuffer ubo, VkDeviceSize uboSize,
                        VkImageView gbuf) {
    recordedThisFrame_ = false;
    if (!frameBegun_ || !pipelinesOk_ || listCount_ == 0 || gbuf == VK_NULL_HANDLE) return;
    frameBegun_ = false;
    if (!setWritten_[slot] || setGbuf_[slot] != gbuf) writeSet(slot, ubo, uboSize, gbuf);

    // Last frame's update wrote the atlas and read the rays; its surfaces and
    // this frame's opaque ones sampled the atlas. All of it before this
    // frame's trace reads the atlas and writes the rays.
    memoryBarrier(cmd,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    VkDescriptorSet sets[2] = {sets_[slot], scene_->descriptorSet()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, sets, 0,
                            nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_);
    vkCmdDispatch(cmd, listCount_, 1, 1);
    ctx_->gpuMark(cmd, "rt gi probe rays");

    // The rays to the update; the trace's reads of the atlas before the
    // update writes it.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, update_);
    vkCmdDispatch(cmd, listCount_, 1, 1);
    ctx_->gpuMark(cmd, "rt gi probe update");

    for (int c = 0; c < gi::kCascades; ++c) gridMin_[c] = nextGridMin_[c];
    haveGrid_ = true;
    recordedThisFrame_ = true;
    sky_ = nextSky_;
    ground_ = nextGround_;
}

void RtGiProbes::recordExposure(VkCommandBuffer cmd, uint32_t slot) {
    if (!recordedThisFrame_) return;
    recordedThisFrame_ = false;
    VkDescriptorSet sets[2] = {sets_[slot], scene_->descriptorSet()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 2, sets, 0,
                            nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposure_);
    vkCmdDispatch(cmd, 1, 1, 1);
}

RtGiProbes::ConsumerData RtGiProbes::consumerData() const {
    ConsumerData d;
    if (!hasResources() || !haveGrid_) return d;
    for (int c = 0; c < gi::kCascades; ++c) {
        d.volume[c] = glm::vec4(glm::vec3(gridMin_[c]), cascadeSpacing(config_, c));
        d.volumeMod[c] = glm::vec4(glm::vec3(gi::storageOf(gridMin_[c])), 0.0f);
    }
    d.sky = glm::vec4(sky_, config_.strength);
    d.ground = glm::vec4(ground_, config_.maxRatio);
    d.params = glm::vec4(1.0f, static_cast<float>(config_.debug), config_.chroma, 0.0f);
    return d;
}

} // namespace rendering
} // namespace wowee
