#include "rendering/rt_scene.hpp"
#include <atomic>
#include <thread>

#include "core/frame_profiler.hpp"
#include "core/logger.hpp"
#include "rendering/vk_texture.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace wowee::rendering {

namespace {

constexpr uint32_t kBindingTriangles = 0;
constexpr uint32_t kBindingNodes = 1;
constexpr uint32_t kBindingMeshes = 2;
constexpr uint32_t kBindingInstances = 3;
constexpr uint32_t kBindingTlasNodes = 4;
constexpr uint32_t kBindingTlas = 5;
constexpr uint32_t kBindingTriExtras = 6;
constexpr uint32_t kBindingMasks = 7;
constexpr uint32_t kBindingDynamicTlas = 8;
constexpr uint32_t kBindingGroups = 9;
constexpr uint32_t kBindingDetails = 10;
constexpr uint32_t kBindingTerrain = 11;

// The software top level begins with three nodes that join the two
// hierarchies: a root over both, then a copy of each one's root as its two
// children (an interior node's children sit side by side). The static nodes
// follow, then the dynamic ones, each pointing at their place in the whole.
constexpr uint32_t kTopHeader = 3;

// A node no ray enters: a point box beyond any distance traced. (An inverted
// box would not do - the slab test reads it as an infinite one.)
RtBvhNode farNode() {
    RtBvhNode n{};
    n.bmin = n.bmax = glm::vec3(1e30f);
    n.leftOrFirst = 0;
    n.count = 0;
    return n;
}

// A hierarchy's root placed in the header, its indices made absolute.
RtBvhNode headerChild(const std::vector<RtBvhNode>& nodes) {
    return nodes.empty() ? farNode() : nodes.front();
}

constexpr uint32_t kInitialTriangles = 1u << 18;
constexpr uint32_t kInitialNodes = 1u << 17;
// Triangles built and uploaded per frame. A binned-SAH build runs at a few
// million triangles a second on one core, so this keeps a frame's share to a
// few milliseconds while the world streams in after the switch is turned on.
constexpr uint64_t kBuildBudgetTriangles = 150000;

uint32_t grownCapacity(uint32_t current, uint32_t needed) {
    uint32_t c = std::max(current, 1024u);
    while (c < needed) c += c / 2;
    return c;
}

RtAabb transformBounds(const RtAabb& b, const glm::mat4& m) {
    RtAabb out;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? b.bmax.x : b.bmin.x, (i & 2) ? b.bmax.y : b.bmin.y,
                               (i & 4) ? b.bmax.z : b.bmin.z);
        out.grow(glm::vec3(m * glm::vec4(corner, 1.0f)));
    }
    return out;
}

}  // namespace

RtScene::~RtScene() { shutdown(); }

bool RtScene::initialize(VkContext* ctx) {
    ctx_ = ctx;
    alive_ = std::make_shared<bool>(true);
    backend_ = ctx->hardwareRayQueryEnabled() ? Backend::Hardware : Backend::Software;
    if (backend_ == Backend::Hardware && !loadHardwareEntryPoints()) {
        LOG_WARNING("RtScene: acceleration structure entry points missing, using the compute tracer");
        backend_ = Backend::Software;
    }

    const VkBufferUsageFlags poolUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    triPool_.elementSize = sizeof(RtTriangle);
    triPool_.usage = poolUsage;
    if (backend_ == Backend::Hardware) {
        triPool_.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    nodePool_.elementSize = sizeof(RtBvhNode);
    nodePool_.usage = poolUsage;
    extraPool_.elementSize = sizeof(glm::uvec4);
    extraPool_.usage = poolUsage;

    placeholder_ = createBuffer(ctx_->getAllocator(), 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VMA_MEMORY_USAGE_CPU_TO_GPU);
    if (!placeholder_.buffer) return false;
    std::memset(placeholder_.info.pMappedData, 0, 64);

    if (!createDescriptors()) return false;
    LOG_INFO("RtScene: ", backend_ == Backend::Hardware ? "hardware" : "software", " backend");
    return true;
}

bool RtScene::loadHardwareEntryPoints() {
    VkDevice dev = ctx_->getDevice();
    auto load = [&](const char* name) { return vkGetDeviceProcAddr(dev, name); };
    pfnGetBufferDeviceAddress_ =
        reinterpret_cast<PFN_vkGetBufferDeviceAddress>(load("vkGetBufferDeviceAddress"));
    pfnCreateAs_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        load("vkCreateAccelerationStructureKHR"));
    pfnDestroyAs_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        load("vkDestroyAccelerationStructureKHR"));
    pfnGetAsBuildSizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        load("vkGetAccelerationStructureBuildSizesKHR"));
    pfnCmdBuildAs_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        load("vkCmdBuildAccelerationStructuresKHR"));
    pfnGetAsAddress_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        load("vkGetAccelerationStructureDeviceAddressKHR"));
    if (!pfnGetBufferDeviceAddress_ || !pfnCreateAs_ || !pfnDestroyAs_ || !pfnGetAsBuildSizes_ ||
        !pfnCmdBuildAs_ || !pfnGetAsAddress_) {
        return false;
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(ctx_->getPhysicalDevice(), &props);
    scratchAlignment_ = std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment, 1);
    return true;
}

bool RtScene::createDescriptors() {
    VkDevice dev = ctx_->getDevice();
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto addBinding = [&](uint32_t binding, VkDescriptorType type) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = binding;
        b.descriptorType = type;
        b.descriptorCount = 1;
        // And the water's fragments, which trace through it
        // (water_rt.glsli).
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    };
    addBinding(kBindingTriangles, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingMeshes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingInstances, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingTriExtras, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingMasks, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingGroups, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingDetails, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    addBinding(kBindingTerrain, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    if (backend_ == Backend::Software) {
        addBinding(kBindingNodes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        addBinding(kBindingTlasNodes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    } else {
        addBinding(kBindingTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
        addBinding(kBindingDynamicTlas, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
    }

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &setLayout_) != VK_SUCCESS) return false;

    std::vector<VkDescriptorPoolSize> sizes;
    sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     (backend_ == Backend::Software ? 8u : 6u) * MAX_FRAMES_IN_FLIGHT});
    if (backend_ == Backend::Hardware) {
        sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2u * MAX_FRAMES_IN_FLIGHT});
    }
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &descriptorPool_) != VK_SUCCESS) return false;

    for (auto& fr : frames_) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptorPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        if (vkAllocateDescriptorSets(dev, &ai, &fr.set) != VK_SUCCESS) return false;
    }
    return true;
}

void RtScene::shutdown() {
    if (!ctx_) return;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    vkDeviceWaitIdle(dev);
    alive_.reset();

    for (auto& m : meshes_) destroyMeshGpu(m);
    meshes_.clear();
    instances_.clear();
    dynInstances_.clear();
    groups_.clear();
    for (auto& fr : frames_) {
        destroyBuffer(alloc, fr.instances);
        destroyBuffer(alloc, fr.tlasNodes);
        destroyBuffer(alloc, fr.groups);
        destroyTlas(fr.staticAs);
        destroyTlas(fr.dynamicAs);
        fr = FrameResources{};
    }
    destroyBuffer(alloc, triPool_.buffer);
    destroyBuffer(alloc, nodePool_.buffer);
    destroyBuffer(alloc, extraPool_.buffer);
    destroyBuffer(alloc, maskBuffer_);
    destroyBuffer(alloc, meshTable_);
    destroyBuffer(alloc, placeholder_);
    if (descriptorPool_) vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

void RtScene::destroyTlas(TlasResources& as) {
    VmaAllocator alloc = ctx_->getAllocator();
    destroyBuffer(alloc, as.asInstances);
    if (as.tlas && pfnDestroyAs_) pfnDestroyAs_(ctx_->getDevice(), as.tlas, nullptr);
    destroyBuffer(alloc, as.tlasBuffer);
    destroyBuffer(alloc, as.scratch);
    as = TlasResources{};
}

void RtScene::destroyMeshGpu(Mesh& m) {
    if (m.blas && pfnDestroyAs_) pfnDestroyAs_(ctx_->getDevice(), m.blas, nullptr);
    m.blas = VK_NULL_HANDLE;
    destroyBuffer(ctx_->getAllocator(), m.blasBuffer);
}

// ---------------------------------------------------------------------------
// Registration

RtScene::MeshId RtScene::addMesh(MeshSource source) {
    if (!ctx_ || source.indices.size() < 3 || source.surfaces.empty()) return kInvalid;
    const size_t triCount = source.indices.size() / 3;
    if (source.surfaces.size() != 1 && source.surfaces.size() != triCount) return kInvalid;
    if (!source.normals.empty() && source.normals.size() != source.positions.size()) source.normals.clear();
    if (!source.oneSided.empty() && source.oneSided.size() != triCount) source.oneSided.clear();
    if (!source.masks.empty() && source.masks.size() != triCount) source.masks.clear();
    if (!source.uvs.empty() && source.uvs.size() != source.positions.size()) source.uvs.clear();
    if (!source.details.empty() && source.details.size() != triCount) source.details.clear();
    if (!source.terrainLayers.empty() && source.terrainLayers.size() != kTerrainRecordWords) {
        source.terrainLayers.clear();
    }
    RtAabb bounds;
    for (uint32_t idx : source.indices) {
        if (idx >= source.positions.size()) return kInvalid;
        bounds.grow(source.positions[idx]);
    }

    MeshId id;
    if (!freeMeshes_.empty()) {
        id = freeMeshes_.back();
        freeMeshes_.pop_back();
    } else {
        id = static_cast<MeshId>(meshes_.size());
        meshes_.emplace_back();
    }
    Mesh& m = meshes_[id];
    m = Mesh{};
    m.live = true;
    m.triCount = static_cast<uint32_t>(triCount);
    m.bounds = bounds;
    m.source = std::move(source);
    // The textures its triangles are coloured by, held while it lives.
    auto hold = [&m, this](uint32_t slot) {
        if (slot == kInvalid) return;
        if (std::find(m.detailRefs.begin(), m.detailRefs.end(), slot) != m.detailRefs.end()) return;
        m.detailRefs.push_back(slot);
        details_.acquire(slot);
    };
    for (uint32_t d : m.source.details) hold(d);
    if (!m.source.terrainLayers.empty()) {
        for (int l = 0; l < 4; ++l) hold(m.source.terrainLayers[size_t(l)]);
        m.terrainSlot = terrainRecords_.add(m.source.terrainLayers.data());
        terrainRecords_.acquire(m.terrainSlot);
        // The record is in the store; the source need not keep it.
        m.source.terrainLayers.clear();
        m.source.terrainLayers.shrink_to_fit();
        meshTableDirty_ = true;
    }
    ++liveMeshes_;
    liveTriangles_ += m.triCount;
    if (active_) {
        m.queued = true;
        pendingMeshes_.push_back(id);
    }
    return id;
}

void RtScene::buildMesh(Mesh& m) {
    const MeshSource& src = m.source;
    m.tris.resize(m.triCount);
    m.hasTranslucent = false;
    for (uint32_t t = 0; t < m.triCount; ++t) {
        const float surface = src.surfaces.size() == 1 ? src.surfaces[0] : src.surfaces[t];
        RtTriangle& tri = m.tris[t];
        uint32_t i0 = src.indices[t * 3], i1 = src.indices[t * 3 + 1], i2 = src.indices[t * 3 + 2];
        // v1.w: 1 when the triangle has a front, which its winding gives
        // (rtClosestEx). The data's own winding is not relied on: the
        // terrain and the buildings are drawn unculled here.
        float front = 0.0f;
        if (!src.normals.empty() && (src.oneSided.empty() || src.oneSided[t])) {
            const glm::vec3 g = glm::cross(src.positions[i1] - src.positions[i0],
                                           src.positions[i2] - src.positions[i0]);
            const glm::vec3 vn = src.normals[i0] + src.normals[i1] + src.normals[i2];
            const float d = glm::dot(g, vn);
            if (d != 0.0f && std::isfinite(d)) {
                if (d < 0.0f) std::swap(i1, i2);
                front = 1.0f;
            }
        }
        tri.v0 = glm::vec4(src.positions[i0], surface);
        tri.v1 = glm::vec4(src.positions[i1], front);
        // v2.w: 1 + the index of the cutout's UVs and mask in the extras
        // pool, as bits; 0 for none. Local here, made absolute on upload,
        // since the hierarchy build reorders the triangles.
        // The extra's .w: the mask in the low half, the detail slot in the
        // high, 0xFFFF for none (rt_trace_alpha.glsli, rt_trace_detail.glsli).
        uint32_t extra = 0;
        if (!src.uvs.empty()) {
            uint32_t mask = 0xFFFFu, detail = 0xFFFFu;
            if (!src.masks.empty() && src.masks[t] < masks_.size() / 64 && src.masks[t] < 0xFFFFu) {
                mask = src.masks[t];
            }
            if (!src.details.empty() && src.details[t] < 0xFFFFu) detail = src.details[t];
            if (mask != 0xFFFFu || detail != 0xFFFFu) {
                m.extras.emplace_back(glm::packHalf2x16(src.uvs[i0]), glm::packHalf2x16(src.uvs[i1]),
                                      glm::packHalf2x16(src.uvs[i2]), mask | (detail << 16));
                extra = static_cast<uint32_t>(m.extras.size());
            }
        }
        tri.v2 = glm::vec4(src.positions[i2], std::bit_cast<float>(extra));
        if (unpackRtSurface(surface).a < 1.0f) m.hasTranslucent = true;
    }
    m.extraCount = static_cast<uint32_t>(m.extras.size());
    // The hardware builds its own hierarchy from the triangles (the BLAS);
    // the one made here is the compute tracer's alone.
    if (backend_ == Backend::Hardware) {
        m.nodeCount = 0;
        m.nodes.clear();
        return;
    }
    RtBvh bvh = buildRtTriangleBvh(m.tris);
    m.nodeCount = static_cast<uint32_t>(bvh.nodes.size());
    m.nodes = std::move(bvh.nodes);
}

void RtScene::setActive(bool active) {
    if (active == active_ || !ctx_) return;
    active_ = active;
    if (!active) {
        releaseGpu();
        return;
    }
    for (MeshId id = 0; id < meshes_.size(); ++id) {
        Mesh& m = meshes_[id];
        if (m.live && !m.queued && !m.uploaded) {
            m.queued = true;
            pendingMeshes_.push_back(id);
        }
    }
    ++sceneVersion_;
}

void RtScene::uploadAllPending() {
    if (!ctx_ || !active_) return;
    // The triangles of every mesh first, on as many threads as there are
    // cores: each mesh is its own, and this is most of the time the upload
    // took on one (4.4 s for a city's 14,000 meshes).
    {
        std::vector<MeshId> todo;
        for (MeshId id : pendingMeshes_) {
            const Mesh& m = meshes_[id];
            if (m.live && m.queued && !m.prebuilt) todo.push_back(id);
        }
        std::atomic<size_t> next{0};
        const auto work = [&]() {
            for (size_t i = next.fetch_add(1); i < todo.size(); i = next.fetch_add(1)) {
                Mesh& m = meshes_[todo[i]];
                buildMesh(m);
                m.prebuilt = true;
            }
        };
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads && todo.size() > 64; ++t) pool.emplace_back(work);
        work();
        for (auto& th : pool) th.join();
    }
    while (!pendingMeshes_.empty()) {
        const size_t before = pendingMeshes_.size();
        VkCommandBuffer cmd = ctx_->beginSingleTimeCommands();
        if (cmd == VK_NULL_HANDLE) return;
        uploadPending(cmd);
        ctx_->endSingleTimeCommands(cmd);
        if (pendingMeshes_.size() >= before) break;  // nothing taken: give up rather than spin
    }
    // Any left over build again from their source when their turn comes.
    for (MeshId id : pendingMeshes_) {
        Mesh& m = meshes_[id];
        if (!m.prebuilt) continue;
        m.prebuilt = false;
        m.tris.clear();
        m.nodes.clear();
        m.extras.clear();
    }
}

void RtScene::releaseGpu() {
    VmaAllocator alloc = ctx_->getAllocator();
    ++gpuEpoch_;
    for (auto& m : meshes_) {
        destroyMeshGpu(m);
        m.uploaded = false;
        m.queued = false;
        m.triOffset = m.nodeOffset = RtRangeAllocator::kFailed;
        m.tris.clear();
        m.tris.shrink_to_fit();
        m.nodes.clear();
        m.nodes.shrink_to_fit();
        m.extras.clear();
        m.extras.shrink_to_fit();
        m.extraOffset = RtRangeAllocator::kFailed;
    }
    pendingMeshes_.clear();
    for (auto& fr : frames_) {
        destroyBuffer(alloc, fr.instances);
        destroyBuffer(alloc, fr.tlasNodes);
        destroyBuffer(alloc, fr.groups);
        destroyTlas(fr.staticAs);
        destroyTlas(fr.dynamicAs);
        const VkDescriptorSet set = fr.set;
        fr = FrameResources{};
        fr.set = set;
    }
    for (GpuPool* p : {&triPool_, &nodePool_, &extraPool_}) {
        destroyBuffer(alloc, p->buffer);
        p->alloc = RtRangeAllocator{};
    }
    // The masks stay on the CPU; the GPU copy is made again when it is needed.
    destroyBuffer(alloc, maskBuffer_);
    maskCapacity_ = 0;
    masksUploaded_ = 0;
    // So are the detail pyramids and the terrain records.
    destroyBuffer(alloc, detailBuffer_);
    detailCapacity_ = 0;
    details_.markAllDirty();
    destroyBuffer(alloc, terrainBuffer_);
    terrainCapacity_ = 0;
    terrainRecords_.markAllDirty();
    destroyBuffer(alloc, meshTable_);
    meshTableCapacity_ = 0;
    meshTableDirty_ = true;
    builtVersion_ = ~0ull;
    ++sceneVersion_;
    ++descriptorGeneration_;
}

void RtScene::removeMesh(MeshId id) {
    if (id >= meshes_.size() || !meshes_[id].live) return;
    Mesh& m = meshes_[id];
    m.live = false;
    --liveMeshes_;
    liveTriangles_ -= m.triCount;
    pendingMeshes_.erase(std::remove(pendingMeshes_.begin(), pendingMeshes_.end(), id),
                         pendingMeshes_.end());

    // In-flight frames may still trace this mesh. Its pool ranges, BLAS and
    // slot are released only once every frame slot has been fenced.
    const uint32_t triOffset = m.triOffset, triCount = m.triCount;
    const uint32_t nodeOffset = m.nodeOffset, nodeCount = m.nodeCount;
    const uint32_t extraOffset = m.extraOffset, extraCount = m.extraCount;
    VkAccelerationStructureKHR blas = m.blas;
    AllocatedBuffer blasBuffer = m.blasBuffer;
    std::vector<uint32_t> detailRefs = std::move(m.detailRefs);
    const uint32_t terrainSlot = m.terrainSlot;
    if (terrainSlot != kInvalid) meshTableDirty_ = true;
    m = Mesh{};
    // The BLAS is destroyed whether or not the scene is still alive by then;
    // the pool ranges and the slot only matter to a scene that is.
    std::weak_ptr<bool> alive = alive_;
    const uint64_t epoch = gpuEpoch_;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    auto destroyAs = pfnDestroyAs_;
    ctx_->deferAfterAllFrameFences([this, alive, epoch, dev, alloc, destroyAs, id, triOffset,
                                    triCount, nodeOffset, nodeCount, extraOffset, extraCount, blas,
                                    blasBuffer, detailRefs, terrainSlot]() mutable {
        if (blas) destroyAs(dev, blas, nullptr);
        destroyBuffer(alloc, blasBuffer);
        if (alive.expired()) return;
        // Its textures, once no frame can still be tracing it: a slot freed
        // here is taken by the next texture added.
        for (uint32_t d : detailRefs) details_.release(d);
        if (terrainSlot != kInvalid) terrainRecords_.release(terrainSlot);
        if (epoch == gpuEpoch_ && triOffset != RtRangeAllocator::kFailed) triPool_.alloc.release(triOffset, triCount);
        if (epoch == gpuEpoch_ && nodeOffset != RtRangeAllocator::kFailed)
            nodePool_.alloc.release(nodeOffset, nodeCount);
        if (epoch == gpuEpoch_ && extraOffset != RtRangeAllocator::kFailed && extraCount > 0)
            extraPool_.alloc.release(extraOffset, extraCount);
        freeMeshes_.push_back(id);
    });
}

RtScene::InstanceId RtScene::addInstance(MeshId mesh, const glm::mat4& objectToWorld) {
    if (mesh >= meshes_.size() || !meshes_[mesh].live) return kInvalid;
    InstanceId id;
    if (!freeInstances_.empty()) {
        id = freeInstances_.back();
        freeInstances_.pop_back();
    } else {
        id = static_cast<InstanceId>(instances_.size());
        instances_.emplace_back();
    }
    instances_[id] = Instance{true, mesh, objectToWorld};
    ++meshes_[mesh].staticRefs;
    ++liveInstances_;
    ++sceneVersion_;
    return id;
}

RtScene::InstanceId RtScene::addDynamicInstance(MeshId mesh, const glm::mat4& objectToWorld,
                                                GroupId group) {
    if (mesh >= meshes_.size() || !meshes_[mesh].live) return kInvalid;
    InstanceId id;
    if (!freeDynInstances_.empty()) {
        id = freeDynInstances_.back();
        freeDynInstances_.pop_back();
    } else {
        id = static_cast<InstanceId>(dynInstances_.size());
        dynInstances_.emplace_back();
    }
    dynInstances_[id] = Instance{true, mesh, objectToWorld, group < groups_.size() ? group : 0};
    ++liveDynInstances_;
    return id | kDynamicBit;
}

void RtScene::setInstanceTransform(InstanceId id, const glm::mat4& objectToWorld) {
    if (isDynamic(id)) {
        // The dynamic top level is rebuilt every frame; nothing to mark.
        const InstanceId d = id & ~kDynamicBit;
        if (d < dynInstances_.size() && dynInstances_[d].live) dynInstances_[d].objectToWorld = objectToWorld;
        return;
    }
    if (id >= instances_.size() || !instances_[id].live) return;
    if (instances_[id].objectToWorld == objectToWorld) return;
    instances_[id].objectToWorld = objectToWorld;
    ++sceneVersion_;
}

void RtScene::removeInstance(InstanceId id) {
    if (isDynamic(id)) {
        const InstanceId d = id & ~kDynamicBit;
        if (d >= dynInstances_.size() || !dynInstances_[d].live) return;
        dynInstances_[d] = Instance{};
        freeDynInstances_.push_back(d);
        --liveDynInstances_;
        return;
    }
    if (id >= instances_.size() || !instances_[id].live) return;
    const MeshId mesh = instances_[id].mesh;
    if (mesh < meshes_.size() && meshes_[mesh].staticRefs > 0) --meshes_[mesh].staticRefs;
    instances_[id] = Instance{};
    freeInstances_.push_back(id);
    --liveInstances_;
    ++sceneVersion_;
}

bool RtScene::meshReady(MeshId mesh) const {
    if (mesh >= meshes_.size()) return false;
    const Mesh& m = meshes_[mesh];
    return m.live && m.uploaded && (backend_ == Backend::Software || m.blas != VK_NULL_HANDLE);
}

RtScene::GroupId RtScene::addGroup() {
    if (groups_.empty()) groups_.emplace_back();  // 0: no group
    GroupId id;
    if (!freeGroups_.empty()) {
        id = freeGroups_.back();
        freeGroups_.pop_back();
    } else {
        id = static_cast<GroupId>(groups_.size());
        groups_.emplace_back();
    }
    groups_[id] = Group{};
    groups_[id].live = true;
    return id;
}

void RtScene::setGroupBounds(GroupId group, const glm::vec3& bmin, const glm::vec3& bmax) {
    if (group == 0 || group >= groups_.size() || !groups_[group].live) return;
    groups_[group].bmin = glm::vec4(bmin, 0.0f);
    groups_[group].bmax = glm::vec4(bmax, 0.0f);
}

void RtScene::removeGroup(GroupId group) {
    if (group == 0 || group >= groups_.size() || !groups_[group].live) return;
    groups_[group] = Group{};
    freeGroups_.push_back(group);
}

// ---------------------------------------------------------------------------
// GPU upkeep

VkDeviceAddress RtScene::bufferAddress(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return pfnGetBufferDeviceAddress_(ctx_->getDevice(), &info);
}

void RtScene::stage(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset, const void* data,
                    VkDeviceSize size) {
    if (size == 0) return;
    AllocatedBuffer staging = createBuffer(ctx_->getAllocator(), size,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           VMA_MEMORY_USAGE_CPU_ONLY);
    if (!staging.buffer) return;
    std::memcpy(staging.info.pMappedData, data, size);
    VkBufferCopy region{0, dstOffset, size};
    vkCmdCopyBuffer(cmd, staging.buffer, dst, 1, &region);
    VmaAllocator alloc = ctx_->getAllocator();
    ctx_->deferAfterFrameFence([alloc, staging]() mutable { destroyBuffer(alloc, staging); });
}

void RtScene::ensurePoolCapacity(VkCommandBuffer cmd, GpuPool& pool, uint32_t needed) {
    if (pool.buffer.buffer && pool.alloc.capacity() >= needed) return;
    const uint32_t newCap = grownCapacity(pool.alloc.capacity(), needed);
    AllocatedBuffer grown = createBuffer(ctx_->getAllocator(),
                                         VkDeviceSize(newCap) * pool.elementSize, pool.usage,
                                         VMA_MEMORY_USAGE_GPU_ONLY);
    if (!grown.buffer) return;
    if (pool.buffer.buffer && pool.alloc.capacity() > 0) {
        VkBufferCopy region{0, 0, VkDeviceSize(pool.alloc.capacity()) * pool.elementSize};
        vkCmdCopyBuffer(cmd, pool.buffer.buffer, grown.buffer, 1, &region);
        VmaAllocator alloc = ctx_->getAllocator();
        AllocatedBuffer old = pool.buffer;
        ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
    }
    pool.buffer = grown;
    pool.alloc.grow(newCap);
    ++descriptorGeneration_;
}

uint32_t RtScene::poolAllocate(VkCommandBuffer cmd, GpuPool& pool, uint32_t count) {
    uint32_t off = pool.alloc.allocate(count);
    if (off != RtRangeAllocator::kFailed) return off;
    ensurePoolCapacity(cmd, pool, pool.alloc.capacity() + count);
    return pool.alloc.allocate(count);
}

void RtScene::uploadPending(VkCommandBuffer cmd) {
    if (pendingMeshes_.empty()) return;
    if (!triPool_.buffer.buffer) ensurePoolCapacity(cmd, triPool_, kInitialTriangles);
    if (backend_ == Backend::Software && !nodePool_.buffer.buffer) {
        ensurePoolCapacity(cmd, nodePool_, kInitialNodes);
    }

    // Build this frame's share from the sources.
    std::vector<MeshId> batch;
    uint64_t built = 0;
    size_t consumed = 0;
    for (; consumed < pendingMeshes_.size() && built < kBuildBudgetTriangles; ++consumed) {
        const MeshId id = pendingMeshes_[consumed];
        Mesh& m = meshes_[id];
        if (!m.live || !m.queued) continue;
        m.queued = false;
        if (!m.prebuilt) buildMesh(m);
        m.prebuilt = false;
        built += m.triCount;
        batch.push_back(id);
    }
    pendingMeshes_.erase(pendingMeshes_.begin(), pendingMeshes_.begin() + consumed);
    if (batch.empty()) return;

    // Allocate everything first: a pool growth copies the old contents, and
    // that copy has to precede the uploads into it.
    std::vector<MeshId> ready;
    for (MeshId id : batch) {
        Mesh& m = meshes_[id];
        m.triOffset = poolAllocate(cmd, triPool_, m.triCount);
        if (backend_ == Backend::Software) m.nodeOffset = poolAllocate(cmd, nodePool_, m.nodeCount);
        if (m.extraCount > 0) m.extraOffset = poolAllocate(cmd, extraPool_, m.extraCount);
        if (m.triOffset == RtRangeAllocator::kFailed ||
            (backend_ == Backend::Software && m.nodeOffset == RtRangeAllocator::kFailed) ||
            (m.extraCount > 0 && m.extraOffset == RtRangeAllocator::kFailed)) {
            LOG_WARNING("RtScene: out of memory for a mesh of ", m.triCount, " triangles");
            if (m.triOffset != RtRangeAllocator::kFailed) triPool_.alloc.release(m.triOffset, m.triCount);
            if (m.nodeOffset != RtRangeAllocator::kFailed) nodePool_.alloc.release(m.nodeOffset, m.nodeCount);
            if (m.extraOffset != RtRangeAllocator::kFailed) extraPool_.alloc.release(m.extraOffset, m.extraCount);
            m.triOffset = m.nodeOffset = m.extraOffset = RtRangeAllocator::kFailed;
            m.tris.clear();
            m.nodes.clear();
            m.extras.clear();
            continue;
        }
        // The cutouts' extras indices, local until now.
        if (m.extraCount > 0) {
            for (RtTriangle& tri : m.tris) {
                const uint32_t local = std::bit_cast<uint32_t>(tri.v2.w);
                if (local != 0) tri.v2.w = std::bit_cast<float>(local + m.extraOffset);
            }
        }
        ready.push_back(id);
    }

    VkMemoryBarrier2 copyOrder{};
    copyOrder.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    copyOrder.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    copyOrder.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    copyOrder.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    copyOrder.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &copyOrder;
    cmdPipelineBarrier2(cmd, dep);

    for (MeshId id : ready) {
        Mesh& m = meshes_[id];
        stage(cmd, triPool_.buffer.buffer, VkDeviceSize(m.triOffset) * sizeof(RtTriangle),
              m.tris.data(), m.tris.size() * sizeof(RtTriangle));
        if (backend_ == Backend::Software) {
            // Child indices are stored relative to the mesh; the shader adds
            // the mesh's node offset, so nothing here needs rewriting.
            stage(cmd, nodePool_.buffer.buffer, VkDeviceSize(m.nodeOffset) * sizeof(RtBvhNode),
                  m.nodes.data(), m.nodes.size() * sizeof(RtBvhNode));
        }
        if (m.extraCount > 0) {
            stage(cmd, extraPool_.buffer.buffer, VkDeviceSize(m.extraOffset) * sizeof(glm::uvec4),
                  m.extras.data(), m.extras.size() * sizeof(glm::uvec4));
        }
    }

    VkMemoryBarrier2 toReaders{};
    toReaders.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    toReaders.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toReaders.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toReaders.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toReaders.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    if (backend_ == Backend::Hardware) {
        toReaders.dstStageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        toReaders.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT;
    }
    dep.pMemoryBarriers = &toReaders;
    cmdPipelineBarrier2(cmd, dep);

    if (backend_ == Backend::Hardware) buildPendingBlas(cmd, ready);

    bool placedStatic = false;
    for (MeshId id : ready) {
        Mesh& m = meshes_[id];
        m.uploaded = true;
        placedStatic = placedStatic || m.staticRefs > 0;
        m.tris.clear();
        m.tris.shrink_to_fit();
        m.nodes.clear();
        m.nodes.shrink_to_fit();
        m.extras.clear();
        m.extras.shrink_to_fit();
    }
    meshTableDirty_ = true;
    // A mesh only dynamic instances place - a character's, a piece of one -
    // reaches the dynamic top level next frame anyway; rebuilding the static
    // one for it cost a city a few milliseconds every few frames.
    if (placedStatic) ++sceneVersion_;
}

void RtScene::buildPendingBlas(VkCommandBuffer cmd, const std::vector<MeshId>& ids) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    const VkDeviceAddress poolAddress = bufferAddress(triPool_.buffer.buffer);

    for (MeshId id : ids) {
        Mesh& m = meshes_[id];

        VkAccelerationStructureGeometryKHR geom{};
        geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // Translucent meshes go through the candidate loop in the shader, which
        // reads the triangle's opacity from the pool.
        geom.flags = m.hasTranslucent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = geom.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = poolAddress + VkDeviceAddress(m.triOffset) * sizeof(RtTriangle);
        tri.vertexStride = sizeof(glm::vec4);
        tri.maxVertex = m.triCount * 3 - 1;
        tri.indexType = VK_INDEX_TYPE_NONE_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR build{};
        build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geom;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        pfnGetAsBuildSizes_(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                            &m.triCount, &sizes);

        m.blasBuffer = createBuffer(alloc, sizes.accelerationStructureSize,
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY);
        AllocatedBuffer scratch = createBuffer(alloc, sizes.buildScratchSize + scratchAlignment_,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                               VMA_MEMORY_USAGE_GPU_ONLY);
        if (!m.blasBuffer.buffer || !scratch.buffer) {
            destroyBuffer(alloc, scratch);
            continue;
        }

        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = m.blasBuffer.buffer;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (pfnCreateAs_(dev, &ci, nullptr, &m.blas) != VK_SUCCESS) {
            destroyBuffer(alloc, scratch);
            continue;
        }

        const VkDeviceAddress scratchAddr = bufferAddress(scratch.buffer);
        build.dstAccelerationStructure = m.blas;
        build.scratchData.deviceAddress =
            (scratchAddr + scratchAlignment_ - 1) & ~(scratchAlignment_ - 1);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = m.triCount;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        pfnCmdBuildAs_(cmd, 1, &build, &ranges);
        ctx_->deferAfterFrameFence([alloc, scratch]() mutable { destroyBuffer(alloc, scratch); });

        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = m.blas;
        m.blasAddress = pfnGetAsAddress_(dev, &ai);
    }

    // BLAS builds must finish before the TLAS build that references them.
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    cmdPipelineBarrier2(cmd, dep);
}

bool RtScene::ensureMapped(AllocatedBuffer& buf, uint32_t& capacity, uint32_t needed,
                           uint32_t elementSize, VkBufferUsageFlags usage) {
    if (buf.buffer && capacity >= needed) return false;
    VmaAllocator alloc = ctx_->getAllocator();
    if (buf.buffer) {
        // This frame slot's previous use has been fenced, but the descriptor
        // set of this slot still names the buffer until it is rewritten below.
        AllocatedBuffer old = buf;
        ctx_->deferAfterFrameFence([alloc, old]() mutable { destroyBuffer(alloc, old); });
    }
    capacity = grownCapacity(capacity, std::max(needed, 1u));
    buf = createBuffer(alloc, VkDeviceSize(capacity) * elementSize, usage,
                       VMA_MEMORY_USAGE_CPU_TO_GPU);
    ++descriptorGeneration_;
    return true;
}

void RtScene::buildStatic() {
    builtVersion_ = sceneVersion_;
    instanceData_.clear();
    asInstanceData_.clear();
    staticBounds_ = RtAabb{};
    std::vector<RtAabb> bounds;
    for (const Instance& inst : instances_) {
        if (!inst.live) continue;
        const Mesh& m = meshes_[inst.mesh];
        if (!m.uploaded) continue;  // failed allocation, or not yet uploaded
        if (backend_ == Backend::Hardware && !m.blas) continue;
        InstanceGPU g{};
        const glm::mat4 inv = glm::inverse(inst.objectToWorld);
        const glm::mat4 invT = glm::transpose(inv);
        g.worldToObject[0] = invT[0];
        g.worldToObject[1] = invT[1];
        g.worldToObject[2] = invT[2];
        g.mesh = inst.mesh;
        g.flags = m.source.flags;
        instanceData_.push_back(g);
        bounds.push_back(transformBounds(m.bounds, inst.objectToWorld));
        staticBounds_.grow(bounds.back());

        if (backend_ == Backend::Hardware) {
            VkAccelerationStructureInstanceKHR ai{};
            const glm::mat4 t = glm::transpose(inst.objectToWorld);
            std::memcpy(&ai.transform, &t, sizeof(ai.transform));
            ai.instanceCustomIndex = static_cast<uint32_t>(instanceData_.size() - 1);
            ai.mask = instanceMask(m.source.flags);
            ai.accelerationStructureReference = m.blasAddress;
            asInstanceData_.push_back(ai);
        }
    }
    tlasNodes_.clear();
    if (backend_ == Backend::Software && !instanceData_.empty()) {
        RtBvh top = buildRtBvh(bounds, 1);
        // Leaves index the reordered instance array; reorder to match.
        std::vector<InstanceGPU> sorted(instanceData_.size());
        for (size_t i = 0; i < top.order.size(); ++i) sorted[i] = instanceData_[top.order[i]];
        instanceData_.swap(sorted);
        tlasNodes_ = std::move(top.nodes);
        // Interior nodes point past the header; leaves index the static rows,
        // which come first.
        for (RtBvhNode& n : tlasNodes_) {
            if (n.count == 0) n.leftOrFirst += kTopHeader;
        }
    }
}

void RtScene::buildDynamic() {
    dynInstanceData_.clear();
    dynAsInstanceData_.clear();
    dynTlasNodes_.clear();
    dynamicBounds_ = RtAabb{};
    const uint32_t firstRow = static_cast<uint32_t>(instanceData_.size());
    std::vector<RtAabb> bounds;
    for (const Instance& inst : dynInstances_) {
        if (!inst.live) continue;
        const Mesh& m = meshes_[inst.mesh];
        if (!m.uploaded) continue;
        if (backend_ == Backend::Hardware && !m.blas) continue;
        InstanceGPU g{};
        const glm::mat4 inv = glm::inverse(inst.objectToWorld);
        const glm::mat4 invT = glm::transpose(inv);
        g.worldToObject[0] = invT[0];
        g.worldToObject[1] = invT[1];
        g.worldToObject[2] = invT[2];
        g.mesh = inst.mesh;
        g.flags = m.source.flags;
        g.group = inst.group < groups_.size() && groups_[inst.group].live ? inst.group : 0;
        dynInstanceData_.push_back(g);
        bounds.push_back(transformBounds(m.bounds, inst.objectToWorld));
        dynamicBounds_.grow(bounds.back());

        if (backend_ == Backend::Hardware) {
            VkAccelerationStructureInstanceKHR ai{};
            const glm::mat4 t = glm::transpose(inst.objectToWorld);
            std::memcpy(&ai.transform, &t, sizeof(ai.transform));
            ai.instanceCustomIndex = firstRow + static_cast<uint32_t>(dynInstanceData_.size() - 1);
            ai.mask = instanceMask(m.source.flags);
            // A grouped instance goes through the shader's candidate loop,
            // which is where a ray from inside its group passes it by.
            if (g.group != 0) ai.flags = VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
            ai.accelerationStructureReference = m.blasAddress;
            dynAsInstanceData_.push_back(ai);
        }
    }
    if (backend_ == Backend::Software && !dynInstanceData_.empty()) {
        RtBvh top = buildRtBvh(bounds, 1);
        std::vector<InstanceGPU> sorted(dynInstanceData_.size());
        for (size_t i = 0; i < top.order.size(); ++i) sorted[i] = dynInstanceData_[top.order[i]];
        dynInstanceData_.swap(sorted);
        dynTlasNodes_ = std::move(top.nodes);
        const uint32_t firstNode = kTopHeader + static_cast<uint32_t>(tlasNodes_.size());
        for (RtBvhNode& n : dynTlasNodes_) {
            n.leftOrFirst += n.count == 0 ? firstNode : firstRow;
        }
    }

    // Group boxes, two vec4 a group; index 0 is no group.
    groupData_.assign(std::max<size_t>(groups_.size(), 1) * 2, glm::vec4(1e30f));
    for (size_t i = 0; i < groups_.size(); ++i) {
        if (!groups_[i].live) continue;
        groupData_[i * 2] = groups_[i].bmin;
        groupData_[i * 2 + 1] = groups_[i].bmax;
    }
}

void RtScene::uploadFrame(VkCommandBuffer cmd, FrameResources& fr) {
    const uint32_t staticCount = static_cast<uint32_t>(instanceData_.size());
    const uint32_t dynamicCount = static_cast<uint32_t>(dynInstanceData_.size());
    fr.instanceCount = staticCount + dynamicCount;

    // A replaced buffer holds nothing: the static part is copied again too.
    if (ensureMapped(fr.instances, fr.instanceCapacity, fr.instanceCount, sizeof(InstanceGPU),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        fr.uploadedVersion = ~0ull;
    }
    if (backend_ == Backend::Software &&
        ensureMapped(fr.tlasNodes, fr.tlasNodeCapacity,
                     kTopHeader + static_cast<uint32_t>(tlasNodes_.size() + dynTlasNodes_.size()),
                     sizeof(RtBvhNode), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        fr.uploadedVersion = ~0ull;
    }
    ensureMapped(fr.groups, fr.groupCapacity, static_cast<uint32_t>(groupData_.size()),
                 sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (fr.groups.buffer && !groupData_.empty()) {
        std::memcpy(fr.groups.info.pMappedData, groupData_.data(), groupData_.size() * sizeof(glm::vec4));
    }

    auto* rows = static_cast<InstanceGPU*>(fr.instances.info.pMappedData);
    const bool staticChanged = fr.uploadedVersion != sceneVersion_;
    if (staticChanged && staticCount > 0) {
        std::memcpy(rows, instanceData_.data(), staticCount * sizeof(InstanceGPU));
    }
    if (dynamicCount > 0) {
        std::memcpy(rows + staticCount, dynInstanceData_.data(), dynamicCount * sizeof(InstanceGPU));
    }

    if (backend_ == Backend::Software) {
        auto* nodes = static_cast<RtBvhNode*>(fr.tlasNodes.info.pMappedData);
        RtBvhNode header[kTopHeader];
        RtAabb all = staticBounds_;
        all.grow(dynamicBounds_);
        header[0] = farNode();
        if (all.valid()) {
            header[0].bmin = all.bmin;
            header[0].bmax = all.bmax;
        }
        header[0].leftOrFirst = 1;
        header[0].count = 0;
        header[1] = headerChild(tlasNodes_);
        header[2] = headerChild(dynTlasNodes_);
        std::memcpy(nodes, header, sizeof(header));
        if (staticChanged && !tlasNodes_.empty()) {
            std::memcpy(nodes + kTopHeader, tlasNodes_.data(), tlasNodes_.size() * sizeof(RtBvhNode));
        }
        if (!dynTlasNodes_.empty()) {
            std::memcpy(nodes + kTopHeader + tlasNodes_.size(), dynTlasNodes_.data(),
                        dynTlasNodes_.size() * sizeof(RtBvhNode));
        }
    } else {
        if (staticChanged || !fr.staticAs.tlas) buildTlas(cmd, fr.staticAs, asInstanceData_, false);
        buildTlas(cmd, fr.dynamicAs, dynAsInstanceData_, true);
    }
    fr.uploadedVersion = sceneVersion_;
}

void RtScene::buildTlas(VkCommandBuffer cmd, TlasResources& as,
                        const std::vector<VkAccelerationStructureInstanceKHR>& data, bool fastBuild) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    const uint32_t count = static_cast<uint32_t>(data.size());

    ensureMapped(as.asInstances, as.asInstanceCapacity, count,
                 sizeof(VkAccelerationStructureInstanceKHR),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    if (count > 0) {
        std::memcpy(as.asInstances.info.pMappedData, data.data(),
                    count * sizeof(VkAccelerationStructureInstanceKHR));
    }

    VkAccelerationStructureGeometryKHR geom{};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.data.deviceAddress = bufferAddress(as.asInstances.buffer);

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    // The dynamic level is built every frame from a few hundred instances;
    // the static one is traced for many frames per build.
    build.flags = fastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                            : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    pfnGetAsBuildSizes_(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count, &sizes);

    if (!as.tlas || as.tlasSize < sizes.accelerationStructureSize) {
        if (as.tlas) {
            VkAccelerationStructureKHR oldAs = as.tlas;
            AllocatedBuffer oldBuf = as.tlasBuffer;
            auto destroyAs = pfnDestroyAs_;
            ctx_->deferAfterFrameFence([dev, alloc, oldAs, oldBuf, destroyAs]() mutable {
                destroyAs(dev, oldAs, nullptr);
                destroyBuffer(alloc, oldBuf);
            });
        }
        // Room to grow, and never nothing: an empty level still needs a
        // structure for the descriptor to name.
        as.tlasSize = std::max<VkDeviceSize>(
            sizes.accelerationStructureSize + sizes.accelerationStructureSize / 2, 4096);
        as.tlasBuffer = createBuffer(alloc, as.tlasSize,
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_GPU_ONLY);
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = as.tlasBuffer.buffer;
        ci.size = as.tlasSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        as.tlas = VK_NULL_HANDLE;
        if (pfnCreateAs_(dev, &ci, nullptr, &as.tlas) != VK_SUCCESS) {
            as.tlas = VK_NULL_HANDLE;
            return;
        }
        ++descriptorGeneration_;
    }
    if (!as.scratch.buffer || as.scratchSize < sizes.buildScratchSize) {
        if (as.scratch.buffer) {
            AllocatedBuffer old = as.scratch;
            ctx_->deferAfterFrameFence([alloc, old]() mutable { destroyBuffer(alloc, old); });
        }
        as.scratchSize = std::max<VkDeviceSize>(sizes.buildScratchSize + sizes.buildScratchSize / 2, 256);
        as.scratch = createBuffer(alloc, as.scratchSize + scratchAlignment_,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VMA_MEMORY_USAGE_GPU_ONLY);
    }

    const VkDeviceAddress scratchAddr = bufferAddress(as.scratch.buffer);
    build.dstAccelerationStructure = as.tlas;
    build.scratchData.deviceAddress = (scratchAddr + scratchAlignment_ - 1) & ~(scratchAlignment_ - 1);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    pfnCmdBuildAs_(cmd, 1, &build, &ranges);

    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    cmdPipelineBarrier2(cmd, dep);
}

void RtScene::writeDescriptors(FrameResources& fr) {
    if (fr.descriptorGeneration == descriptorGeneration_) return;
    fr.descriptorGeneration = descriptorGeneration_;

    auto info = [&](const AllocatedBuffer& b) {
        return VkDescriptorBufferInfo{b.buffer ? b.buffer : placeholder_.buffer, 0, VK_WHOLE_SIZE};
    };
    VkDescriptorBufferInfo tris = info(triPool_.buffer);
    VkDescriptorBufferInfo nodes = info(nodePool_.buffer);
    VkDescriptorBufferInfo meshes = info(meshTable_);
    VkDescriptorBufferInfo insts = info(fr.instances);
    VkDescriptorBufferInfo tlasNodes = info(fr.tlasNodes);
    VkDescriptorBufferInfo extras = info(extraPool_.buffer);
    VkDescriptorBufferInfo masks = info(maskBuffer_);
    VkDescriptorBufferInfo groups = info(fr.groups);
    VkDescriptorBufferInfo details = info(detailBuffer_);
    VkDescriptorBufferInfo terrain = info(terrainBuffer_);

    std::vector<VkWriteDescriptorSet> writes;
    auto add = [&](uint32_t binding, const VkDescriptorBufferInfo* bi) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = fr.set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = bi;
        writes.push_back(w);
    };
    add(kBindingTriangles, &tris);
    add(kBindingMeshes, &meshes);
    add(kBindingInstances, &insts);
    add(kBindingTriExtras, &extras);
    add(kBindingMasks, &masks);
    add(kBindingGroups, &groups);
    add(kBindingDetails, &details);
    add(kBindingTerrain, &terrain);
    VkWriteDescriptorSetAccelerationStructureKHR asWrite[2]{};
    if (backend_ == Backend::Software) {
        add(kBindingNodes, &nodes);
        add(kBindingTlasNodes, &tlasNodes);
    } else {
        const std::pair<uint32_t, VkAccelerationStructureKHR*> levels[2] = {
            {kBindingTlas, &fr.staticAs.tlas}, {kBindingDynamicTlas, &fr.dynamicAs.tlas}};
        for (int i = 0; i < 2; ++i) {
            if (!*levels[i].second) continue;
            asWrite[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asWrite[i].accelerationStructureCount = 1;
            asWrite[i].pAccelerationStructures = levels[i].second;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.pNext = &asWrite[i];
            w.dstSet = fr.set;
            w.dstBinding = levels[i].first;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes.push_back(w);
        }
    }
    vkUpdateDescriptorSets(ctx_->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void RtScene::update(VkCommandBuffer cmd) {
    if (!ctx_ || !active_) return;
    uploadMasks(cmd);
    details_.collect();
    uploadSlots(cmd, details_, detailBuffer_, detailCapacity_);
    uploadSlots(cmd, terrainRecords_, terrainBuffer_, terrainCapacity_);
    {
        // What the traced hits' textures hold, when it has moved by a megabyte.
        const uint64_t bytes = details_.bytes() + terrainRecords_.bytes();
        if (bytes / (1u << 20) != loggedDetailMiB_) {
            loggedDetailMiB_ = bytes / (1u << 20);
            LOG_INFO("RtScene: hit textures ", details_.live(), " detail pyramids in ", details_.capacity(),
                     " slots (", details_.bytes() >> 10, " KB), ", terrainRecords_.live(),
                     " terrain records in ", terrainRecords_.capacity(), " slots (", terrainRecords_.bytes() >> 10,
                     " KB); textures' own copies ", VkTexture::rtDetailBytes() >> 10, " KB");
        }
    }
    uploadPending(cmd);

    if (meshTableDirty_) {
        meshTableDirty_ = false;
        std::vector<MeshGPU> table(meshes_.size());
        for (size_t i = 0; i < meshes_.size(); ++i) {
            // The third word: 1 + the terrain record, 0 for none.
            const uint32_t terrain = meshes_[i].live && meshes_[i].terrainSlot != kInvalid
                                         ? meshes_[i].terrainSlot + 1
                                         : 0;
            table[i] = {meshes_[i].nodeOffset, meshes_[i].triOffset, terrain, 0};
        }
        if (!meshTable_.buffer || meshTableCapacity_ < table.size()) {
            if (meshTable_.buffer) {
                VmaAllocator alloc = ctx_->getAllocator();
                AllocatedBuffer old = meshTable_;
                ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
            }
            meshTableCapacity_ = grownCapacity(meshTableCapacity_, static_cast<uint32_t>(table.size()));
            meshTable_ = createBuffer(ctx_->getAllocator(),
                                      VkDeviceSize(meshTableCapacity_) * sizeof(MeshGPU),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_GPU_ONLY);
            ++descriptorGeneration_;
        }
        // Rows of live meshes never change after upload and freed rows are
        // unreferenced, so rewriting the table while an earlier frame reads
        // it only changes rows no one is looking at.
        stage(cmd, meshTable_.buffer, 0, table.data(), table.size() * sizeof(MeshGPU));
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        cmdPipelineBarrier2(cmd, dep);
    }

    // The static data once per version of that set, the dynamic every frame.
    if (builtVersion_ != sceneVersion_) {
        WOWEE_PROFILE_SCOPE("rt: static top level", Cpu);
        buildStatic();
    }
    {
        WOWEE_PROFILE_SCOPE("rt: dynamic top level", Cpu);
        buildDynamic();
    }
    FrameResources& fr = frames_[ctx_->getCurrentFrame()];
    uploadFrame(cmd, fr);
    writeDescriptors(fr);
}

uint32_t RtScene::addAlphaMask(const std::array<uint64_t, 64>& bits) {
    // FNV-1a over the rows; a collision would only share a mask between two
    // textures that look the same.
    uint64_t h = 1469598103934665603ull;
    for (uint64_t row : bits) {
        h ^= row;
        h *= 1099511628211ull;
    }
    if (auto it = maskByHash_.find(h); it != maskByHash_.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(masks_.size() / 64);
    masks_.insert(masks_.end(), bits.begin(), bits.end());
    maskByHash_.emplace(h, id);
    return id;
}

void RtScene::uploadMasks(VkCommandBuffer cmd) {
    const uint32_t count = static_cast<uint32_t>(masks_.size() / 64);
    if (count == masksUploaded_) return;
    if (!maskBuffer_.buffer || maskCapacity_ < count) {
        if (maskBuffer_.buffer) {
            VmaAllocator alloc = ctx_->getAllocator();
            AllocatedBuffer old = maskBuffer_;
            ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
        }
        maskCapacity_ = grownCapacity(maskCapacity_, count);
        maskBuffer_ = createBuffer(ctx_->getAllocator(), VkDeviceSize(maskCapacity_) * 64 * sizeof(uint64_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_MEMORY_USAGE_GPU_ONLY);
        masksUploaded_ = 0;  // a new buffer holds none of them
        ++descriptorGeneration_;
        if (!maskBuffer_.buffer) {
            maskCapacity_ = 0;
            return;
        }
    }
    stage(cmd, maskBuffer_.buffer, VkDeviceSize(masksUploaded_) * 64 * sizeof(uint64_t),
          masks_.data() + size_t(masksUploaded_) * 64, size_t(count - masksUploaded_) * 64 * sizeof(uint64_t));
    masksUploaded_ = count;
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    cmdPipelineBarrier2(cmd, dep);
}

uint32_t RtScene::addDetail(const std::vector<uint32_t>& pyramid) {
    if (pyramid.size() != rt_detail::kSlotWords) return kInvalid;
    // The extras keep the slot in 16 bits.
    if (details_.capacity() >= 0xFFFFu && details_.live() >= details_.capacity()) return kInvalid;
    const uint32_t slot = details_.add(pyramid.data());
    return slot < 0xFFFFu ? slot : kInvalid;
}

std::vector<uint32_t> RtScene::terrainRecord(const TerrainLayers& layers) {
    std::vector<uint32_t> r(kTerrainRecordWords, 0);
    for (int l = 0; l < 4; ++l) r[size_t(l)] = layers.details[l];
    r[4] = (layers.weighted ? 1u : 0u) | (layers.hasLayer[0] ? 2u : 0u) | (layers.hasLayer[1] ? 4u : 0u) |
           (layers.hasLayer[2] ? 8u : 0u);
    r[5] = std::bit_cast<uint32_t>(layers.maxX);
    r[6] = std::bit_cast<uint32_t>(layers.maxY);
    r[7] = std::bit_cast<uint32_t>(layers.size);
    r[8] = std::bit_cast<uint32_t>(layers.texScale);
    if (layers.shading.size() == 81) {
        r[4] |= 16u;
        for (size_t i = 0; i < 81; ++i) {
            const glm::vec3 c = glm::clamp(layers.shading[i] * 0.5f, glm::vec3(0.0f), glm::vec3(1.0f));
            r[16 + 256 + i] = glm::packUnorm4x8(glm::vec4(c, 1.0f));
        }
    }
    // Each 64x64 map at 16x16, the mean of sixteen: 2 yards a texel, which
    // the reflection's waves blur past; 32x32 held 4 KB a chunk, 51 MB for
    // the 49 tiles loaded around a city.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint32_t word = 0;
            for (int l = 0; l < 3; ++l) {
                const std::vector<uint8_t>* a = layers.alpha[l];
                if (!a || a->size() < 64 * 64) {
                    // No map: the layer covers all of the chunk (terrain_renderer).
                    if (layers.hasLayer[l]) word |= 255u << (8 * l);
                    continue;
                }
                uint32_t sum = 0;
                for (int dy = 0; dy < 4; ++dy) {
                    for (int dx = 0; dx < 4; ++dx) sum += (*a)[size_t(y * 4 + dy) * 64 + size_t(x * 4 + dx)];
                }
                word |= ((sum + 8) / 16) << (8 * l);
            }
            r[16 + size_t(y) * 16 + size_t(x)] = word;
        }
    }
    return r;
}

void RtScene::uploadSlots(VkCommandBuffer cmd, RtSlotStore& store, AllocatedBuffer& buffer, uint32_t& capacity) {
    const uint32_t count = store.capacity();
    if (count == 0) return;
    const VkDeviceSize slotBytes = VkDeviceSize(store.slotWords()) * sizeof(uint32_t);
    bool fresh = false;
    if (!buffer.buffer || capacity < count) {
        if (buffer.buffer) {
            VmaAllocator alloc = ctx_->getAllocator();
            AllocatedBuffer old = buffer;
            ctx_->deferAfterAllFrameFences([alloc, old]() mutable { destroyBuffer(alloc, old); });
        }
        capacity = grownCapacity(capacity, count);
        buffer = createBuffer(ctx_->getAllocator(), VkDeviceSize(capacity) * slotBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VMA_MEMORY_USAGE_GPU_ONLY);
        ++descriptorGeneration_;
        fresh = true;
        if (!buffer.buffer) {
            capacity = 0;
            return;
        }
    }
    std::vector<uint32_t> dirty = store.takeDirty();
    if (fresh) {
        // A new buffer holds none of them: all of it, in one copy.
        stage(cmd, buffer.buffer, 0, store.words().data(), VkDeviceSize(count) * slotBytes);
    } else {
        if (dirty.empty()) return;
        for (uint32_t s : dirty) stage(cmd, buffer.buffer, VkDeviceSize(s) * slotBytes, store.slot(s), slotBytes);
    }
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    cmdPipelineBarrier2(cmd, dep);
}

VkDescriptorSet RtScene::descriptorSet() const {
    return ctx_ ? frames_[ctx_->getCurrentFrame()].set : VK_NULL_HANDLE;
}

RtScene::Stats RtScene::stats() const {
    Stats s;
    s.meshes = liveMeshes_;
    s.instances = liveInstances_;
    s.dynamicInstances = liveDynInstances_;
    s.triangles = liveTriangles_;
    auto sz = [](const AllocatedBuffer& b) { return b.buffer ? uint64_t(b.info.size) : 0ull; };
    s.gpuBytes = sz(triPool_.buffer) + sz(nodePool_.buffer) + sz(extraPool_.buffer) +
                 sz(maskBuffer_) + sz(meshTable_);
    for (const auto& m : meshes_) s.gpuBytes += sz(m.blasBuffer);
    return s;
}

}  // namespace wowee::rendering
