// Ground cover by chunk: the static store behind M2Renderer::registerDetailChunk.
//
// A chunk's tufts are placed once (TerrainManager::updateDetailDoodads) and
// never move, so what each one draws with - its model matrix, the terrain's
// colour and normal under it, its flags - is written here once, into a
// persistent buffer. The frame then tests each chunk as a whole and lists
// the entries it draws (renderClutterChunks); the bones' base is the only
// part of an entry that changes from frame to frame, and it goes in that list.
#include "rendering/m2_renderer.hpp"

#include "rendering/m2_renderer_internal.h"
#include "rendering/vk_context.hpp"
#include "core/env_flag.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_set>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

bool makeBuffer(VmaAllocator allocator, VkDeviceSize size, ::VkBuffer& buffer, VmaAllocation& alloc,
                void*& mapped) {
    VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &bci, &aci, &buffer, &alloc, &info) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        alloc = VK_NULL_HANDLE;
        mapped = nullptr;
        return false;
    }
    mapped = info.pMappedData;
    return true;
}

// A freed range is handed out again only once no frame still in flight can
// be reading it: this many frames after it was freed.
constexpr uint64_t kClutterFreeDelayFrames = 3;

}  // namespace

bool M2Renderer::initClutterBuffers() {
    if (!vkCtx_) return false;
    // One slot each: the bindings exist from the start, and the store grows
    // when the first chunk is placed.
    clutterStaticCapacity_ = 1;
    clutterStaticCpu_.assign(1, M2InstanceGPU{});
    if (!makeBuffer(vkCtx_->getAllocator(), sizeof(M2InstanceGPU), clutterStaticBuffer_,
                    clutterStaticAlloc_, clutterStaticMapped_)) {
        return false;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        clutterListCapacity_[i] = 1;
        if (!makeBuffer(vkCtx_->getAllocator(), sizeof(glm::ivec2), clutterListBuffer_[i],
                        clutterListAlloc_[i], clutterListMapped_[i])) {
            return false;
        }
    }
    return true;
}

void M2Renderer::destroyClutterBuffers() {
    if (!vkCtx_) return;
    VmaAllocator allocator = vkCtx_->getAllocator();
    if (clutterStaticBuffer_) vmaDestroyBuffer(allocator, clutterStaticBuffer_, clutterStaticAlloc_);
    clutterStaticBuffer_ = VK_NULL_HANDLE;
    clutterStaticAlloc_ = VK_NULL_HANDLE;
    clutterStaticMapped_ = nullptr;
    clutterStaticCapacity_ = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        if (clutterListBuffer_[i]) vmaDestroyBuffer(allocator, clutterListBuffer_[i], clutterListAlloc_[i]);
        clutterListBuffer_[i] = VK_NULL_HANDLE;
        clutterListAlloc_[i] = VK_NULL_HANDLE;
        clutterListMapped_[i] = nullptr;
        clutterListCapacity_[i] = 0;
    }
    clutterStaticCpu_.clear();
    resetClutterChunks();
}

bool M2Renderer::growClutterStatic(uint32_t needed) {
    if (!vkCtx_) return false;
    const uint32_t capacity = std::max({needed, clutterStaticCapacity_ * 2u, 16384u});
    ::VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!makeBuffer(vkCtx_->getAllocator(), VkDeviceSize(capacity) * sizeof(M2InstanceGPU), buffer, alloc,
                    mapped)) {
        LOG_WARNING("Ground cover: could not grow its store to ", capacity, " entries");
        return false;
    }
    clutterStaticCpu_.resize(capacity);
    if (clutterStaticEnd_ > 0) {
        std::memcpy(mapped, clutterStaticCpu_.data(), size_t(clutterStaticEnd_) * sizeof(M2InstanceGPU));
    }
    // Both frames in flight may still read the old one.
    if (clutterStaticBuffer_) {
        VmaAllocator allocator = vkCtx_->getAllocator();
        ::VkBuffer oldBuffer = clutterStaticBuffer_;
        VmaAllocation oldAlloc = clutterStaticAlloc_;
        vkCtx_->deferAfterAllFrameFences([allocator, oldBuffer, oldAlloc]() {
            vmaDestroyBuffer(allocator, oldBuffer, oldAlloc);
        });
    }
    clutterStaticBuffer_ = buffer;
    clutterStaticAlloc_ = alloc;
    clutterStaticMapped_ = mapped;
    clutterStaticCapacity_ = capacity;
    ++clutterStaticGen_;
    LOG_INFO("Ground cover: chunk store holds ", capacity, " entries (",
             (VkDeviceSize(capacity) * sizeof(M2InstanceGPU)) >> 20, " MB)");
    return true;
}

bool M2Renderer::ensureClutterList(uint32_t frameIndex, uint32_t needed) {
    if (frameIndex >= 2 || !vkCtx_) return false;
    if (clutterListCapacity_[frameIndex] >= needed) return true;
    const uint32_t capacity = std::max(needed + needed / 4, 4096u);
    ::VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!makeBuffer(vkCtx_->getAllocator(), VkDeviceSize(capacity) * sizeof(glm::ivec2), buffer, alloc, mapped)) {
        LOG_WARNING("Ground cover: could not grow the frame's list to ", capacity, " entries");
        return false;
    }
    // This slot's last use is fenced; the queue frees it after this one's.
    if (clutterListBuffer_[frameIndex]) {
        VmaAllocator allocator = vkCtx_->getAllocator();
        ::VkBuffer oldBuffer = clutterListBuffer_[frameIndex];
        VmaAllocation oldAlloc = clutterListAlloc_[frameIndex];
        vkCtx_->deferAfterFrameFence([allocator, oldBuffer, oldAlloc]() {
            vmaDestroyBuffer(allocator, oldBuffer, oldAlloc);
        });
    }
    clutterListBuffer_[frameIndex] = buffer;
    clutterListAlloc_[frameIndex] = alloc;
    clutterListMapped_[frameIndex] = mapped;
    clutterListCapacity_[frameIndex] = capacity;
    ++clutterListGen_[frameIndex];
    return true;
}

void M2Renderer::bindClutterBuffers(uint32_t frameIndex) {
    if (frameIndex >= 2 || !vkCtx_ || !instanceSet_[frameIndex]) return;
    if (!clutterStaticBuffer_ || !clutterListBuffer_[frameIndex]) return;
    if (instanceSetStaticGen_[frameIndex] == clutterStaticGen_ &&
        instanceSetListGen_[frameIndex] == clutterListGen_[frameIndex]) {
        return;
    }
    const VkDescriptorBufferInfo listInfo{clutterListBuffer_[frameIndex], 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo staticInfo{clutterStaticBuffer_, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t w = 0; w < 2; ++w) {
        writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[w].dstSet = instanceSet_[frameIndex];
        writes[w].dstBinding = 1 + w;
        writes[w].descriptorCount = 1;
        writes[w].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].pBufferInfo = &listInfo;
    writes[1].pBufferInfo = &staticInfo;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
    instanceSetStaticGen_[frameIndex] = clutterStaticGen_;
    instanceSetListGen_[frameIndex] = clutterListGen_[frameIndex];
}

uint32_t M2Renderer::allocClutterRange(uint32_t count) {
    for (auto it = clutterFree_.begin(); it != clutterFree_.end(); ++it) {
        if (it->second < count) continue;
        const uint32_t first = it->first;
        const uint32_t rest = it->second - count;
        clutterFree_.erase(it);
        if (rest > 0) clutterFree_.emplace(first + count, rest);
        return first;
    }
    if (clutterStaticEnd_ + count > clutterStaticCapacity_ && !growClutterStatic(clutterStaticEnd_ + count)) {
        return UINT32_MAX;
    }
    const uint32_t first = clutterStaticEnd_;
    clutterStaticEnd_ += count;
    return first;
}

void M2Renderer::freeClutterRange(uint32_t first, uint32_t count) {
    if (count == 0) return;
    clutterPendingFree_.push_back({clutterFrame_, first, count});
}

void M2Renderer::reclaimClutterRanges() {
    ++clutterFrame_;
    size_t keep = 0;
    for (size_t i = 0; i < clutterPendingFree_.size(); ++i) {
        const auto pending = clutterPendingFree_[i];
        if (pending.frame + kClutterFreeDelayFrames > clutterFrame_) {
            clutterPendingFree_[keep++] = pending;
            continue;
        }
        // Into the free list, joined with its neighbours.
        auto it = clutterFree_.emplace(pending.first, pending.count).first;
        auto next = std::next(it);
        if (next != clutterFree_.end() && it->first + it->second == next->first) {
            it->second += next->second;
            clutterFree_.erase(next);
        }
        if (it != clutterFree_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second == it->first) {
                prev->second += it->second;
                clutterFree_.erase(it);
                it = prev;
            }
        }
        // The last range given back shortens the store's used part instead.
        if (it->first + it->second == clutterStaticEnd_) {
            clutterStaticEnd_ = it->first;
            clutterFree_.erase(it);
        }
    }
    clutterPendingFree_.resize(keep);
}

void M2Renderer::writeClutterEntry(const M2Instance& inst) {
    if (inst.clutterChunk == 0 || inst.clutterSlot >= clutterStaticCapacity_ || !inst.cachedModel) return;
    static const bool noSkinning = core::envFlagEnabled("WOWEE_M2_NO_SKINNING");
    const M2ModelGPU& model = *inst.cachedModel;
    const bool animated = model.hasAnimation && !model.disableAnimation;
    // As the instance-at-a-time pass writes it (renderOpaque), its distance
    // fade 1: ground cover fades in its shader, by view depth.
    M2InstanceGPU e{};
    e.model = inst.modelMatrix;
    e.uvOffset = glm::vec2(0.0f);
    e.uvLinear = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    e.uvOffset2 = glm::vec4(0.0f);
    e.uvLinear2 = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    e.colorMul = glm::vec4(1.0f, 1.0f, 1.0f, -1.0f);
    e.fadeAlpha = 1.0f;
    e.useBones = (animated && !noSkinning) ? 1 : 0;
    e.boneBase = 0;  // the frame's, from its list
    e.boneCount = static_cast<int32_t>(std::min(model.bones.size(), size_t(kMaxBonesPerInstance)));
    e.highlight = inst.highlight;
    writeInstanceLight(e, inst);
    clutterStaticCpu_[inst.clutterSlot] = e;
    if (clutterStaticMapped_) {
        std::memcpy(static_cast<M2InstanceGPU*>(clutterStaticMapped_) + inst.clutterSlot, &e, sizeof(e));
    }
}

void M2Renderer::registerDetailChunk(const std::vector<uint32_t>& instanceIds) {
    if (!vkCtx_ || !clutterStaticBuffer_ || instanceIds.empty()) return;
    struct Item {
        uint32_t modelId;
        uint32_t id;
        size_t index;
    };
    std::vector<Item> items;
    items.reserve(instanceIds.size());
    std::unordered_map<const M2ModelGPU*, bool> eligible;
    for (uint32_t id : instanceIds) {
        auto idxIt = instanceIndexById.find(id);
        if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) continue;
        const M2Instance& inst = instances[idxIt->second];
        if (!inst.detailDoodad || inst.clutterChunk != 0 || !inst.cachedModel || !inst.cachedIsValid) continue;
        if (inst.isGameObject || inst.fade < 0.999f) continue;
        auto [it, fresh] = eligible.emplace(inst.cachedModel, false);
        if (fresh) {
            const char* why = clutterUnchunkable(*inst.cachedModel);
            it->second = why == nullptr;
            // Said once a model: which ground cover stays an instance at a time.
            static std::unordered_set<const M2ModelGPU*> said;
            if (why && said.insert(inst.cachedModel).second) {
                LOG_INFO("Ground cover: '", inst.cachedModel->name, "' drawn an instance at a time (", why, ")");
            }
        }
        if (!it->second) continue;
        items.push_back({inst.modelId, id, idxIt->second});
    }
    if (items.empty()) return;
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.modelId < b.modelId; });

    const auto count = static_cast<uint32_t>(items.size());
    const uint32_t first = allocClutterRange(count);
    if (first == UINT32_MAX) return;

    uint32_t handle = nextClutterChunk_++;
    if (handle == 0) handle = nextClutterChunk_++;
    ClutterChunk chunk;
    chunk.first = first;
    chunk.count = count;
    chunk.ids.reserve(count);
    chunk.boxMin = glm::vec3(std::numeric_limits<float>::max());
    chunk.boxMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (uint32_t k = 0; k < count; ++k) {
        M2Instance& inst = instances[items[k].index];
        const M2ModelGPU& model = *inst.cachedModel;
        if (chunk.groups.empty() || chunk.groups.back().modelId != inst.modelId) {
            ClutterGroup group;
            group.model = &model;
            group.modelId = inst.modelId;
            group.first = first + k;
            group.animated = model.hasAnimation && !model.disableAnimation;
            chunk.groups.push_back(std::move(group));
        }
        ClutterGroup& group = chunk.groups.back();
        ++group.count;
        if (group.animated) group.ids.push_back(inst.id);
        chunk.ids.push_back(inst.id);
        // The box of what it draws: the cull sphere's centre and the visual
        // extent the instance-at-a-time cull took it from.
        const glm::vec3 half = inst.cachedVisualExtent * 0.5f;
        chunk.boxMin = glm::min(chunk.boxMin, inst.cachedCullCenter - half);
        chunk.boxMax = glm::max(chunk.boxMax, inst.cachedCullCenter + half);
        inst.clutterChunk = handle;
        inst.clutterSlot = first + k;
        writeClutterEntry(inst);
    }
    clutterLive_ += count;
    clutterChunks_.emplace(handle, std::move(chunk));
}

void M2Renderer::releaseDetailChunk(uint32_t handle) {
    auto it = clutterChunks_.find(handle);
    if (it == clutterChunks_.end()) return;
    for (uint32_t id : it->second.ids) {
        auto idxIt = instanceIndexById.find(id);
        if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) continue;
        M2Instance& inst = instances[idxIt->second];
        if (inst.clutterChunk == handle) inst.clutterChunk = 0;
    }
    freeClutterRange(it->second.first, it->second.count);
    clutterLive_ -= std::min(clutterLive_, it->second.count);
    clutterChunks_.erase(it);
}

void M2Renderer::resetClutterChunks() {
    // Only where the device is idle (clear, clearInstances, shutdown): every
    // slot is free at once.
    clutterChunks_.clear();
    clutterFree_.clear();
    clutterPendingFree_.clear();
    clutterStaticEnd_ = 0;
    clutterLive_ = 0;
    clutterDraws_.clear();
}

}  // namespace rendering
}  // namespace wowee
