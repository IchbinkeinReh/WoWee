#pragma once

/// The world as the ray traced lighting sees it.
///
/// Renderers register geometry here as they load it - a mesh per terrain
/// chunk, WMO group or M2 model, in its own object space - and an instance for
/// every placement of it. Each frame `update` uploads what changed and brings
/// the acceleration structures up to date, and the trace shaders bind
/// `descriptorSet()` at set 1.
///
/// Two backends behind the one interface:
///  - software: a two-level BVH walked in compute (`rt_trace_sw.glsli`). The
///    bottom levels are built on the CPU when a mesh is added; the top level
///    is rebuilt on the CPU when the instance set changes.
///  - hardware: VK_KHR_acceleration_structure + ray queries
///    (`rt_trace_hw.glsli`). The BLAS of each mesh is built from the same
///    triangle pool the software path reads, so hit lookups are identical.
///
/// Geometry is kept as compact indexed source meshes whether or not the
/// lighting is on. Only while it is active are BVHs built and the pools
/// filled - a few hundred thousand triangles a frame, so switching it on
/// streams the world in rather than stalling - and switching it off frees
/// every GPU byte.
///
/// Instances come in two kinds. Static ones - the terrain, the buildings, the
/// doodads - are gathered into one top level that is rebuilt only when that
/// set changes. Dynamic ones - characters and their parts, transports, what
/// animates - are expected to move every frame and get a top level of their
/// own, rebuilt every frame from just them; the tracers walk both. A moving
/// unit then costs a frame what the few hundred dynamic instances cost, not
/// what the hundred thousand static ones of a city would.
///
/// A dynamic instance may name a group: a box in the world, set every frame,
/// from inside which rays do not see the group's own instances. A character
/// is one group - body, parts and what it carries - so it never shadows or
/// occludes itself, as the shadow map never lets a model shadow itself
/// either (CharPushConstants::lightFlags). Its proxy is only an approximation
/// of the skinned surface on screen and would otherwise lay streaks of
/// shadow over it.
///
/// Main thread only. Removing a mesh that instances still use is a caller bug.

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "rendering/rt_bvh.hpp"
#include "rendering/rt_range_allocator.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

namespace wowee::rendering {

class RtScene {
public:
    using MeshId = uint32_t;
    using InstanceId = uint32_t;
    using GroupId = uint32_t;
    static constexpr uint32_t kInvalid = ~0u;
    /// Set in the ids of dynamic instances.
    static constexpr InstanceId kDynamicBit = 0x80000000u;

    enum class Backend { Software, Hardware };

    RtScene() = default;
    ~RtScene();
    RtScene(const RtScene&) = delete;
    RtScene& operator=(const RtScene&) = delete;

    bool initialize(VkContext* ctx);
    void shutdown();

    [[nodiscard]] Backend backend() const { return backend_; }

    /// What a trace shader may need to know of a mesh beyond its triangles,
    /// read per instance (RtInstance::flags in rt_trace_*.glsli).
    enum MeshFlags : uint32_t {
        /// Ground: a heightfield whose every face looks up, so a ray that
        /// meets it going upward is coming from underneath (rtClosestEx).
        kMeshTerrain = 1u,
        /// A building's own faces (a WMO's): its floors, walls and roofs.
        kMeshBuilding = 2u,
    };

    /// The ray masks of the hardware tracer's instances, by what they are.
    /// Every instance answers a ray of mask kRayAll; only the ground and the
    /// buildings - what divides one space from another - answer a ray of
    /// kRayStructure (rt_trace.comp.glsl: a point light that one of these
    /// stops gives nothing, whatever the point shadows' strength). The
    /// software tracer reads the mesh flags instead (rtStructureOnly).
    static constexpr uint8_t kRayAll = 0xFF;
    static constexpr uint8_t kRayStructure = 0x02;
    [[nodiscard]] static constexpr uint8_t instanceMask(uint32_t meshFlags) {
        return (meshFlags & (kMeshTerrain | kMeshBuilding)) != 0 ? uint8_t(0x01 | kRayStructure) : uint8_t(0x01);
    }

    /// An indexed triangle mesh in object space. `surfaces` holds one packed
    /// surface (packRtSurface) per triangle, or a single one for all of them.
    ///
    /// With `normals` (one per position), each triangle is wound so that its
    /// geometric normal points the way its vertex normals do, and is marked
    /// as having a front - unless `oneSided` is given and says it has none
    /// (a two-sided material). A ray that meets such a triangle from behind
    /// started inside a solid; the probe volume switches such probes off.
    struct MeshSource {
        std::vector<glm::vec3> positions;
        std::vector<uint32_t> indices;
        std::vector<float> surfaces;
        std::vector<glm::vec3> normals;
        std::vector<uint8_t> oneSided;  // per triangle; empty is all of them
        /// A cutout: with `uvs` (one per position), a triangle whose `masks`
        /// entry names an alpha mask (addAlphaMask) is hit only where the
        /// mask says its texture is visible, as the shadow map's alpha test
        /// has it. Others take their surface's opacity as a probability.
        std::vector<glm::vec2> uvs;
        std::vector<uint32_t> masks;  // per triangle; kInvalid for none
        uint32_t flags = 0;  // MeshFlags
    };

    /// A texture's 64x64 visibility (VkTexture::alphaMask), for MeshSource::
    /// masks. The same mask given twice is stored once; masks are kept for
    /// the scene's life - a few hundred bytes each, a few hundred textures.
    uint32_t addAlphaMask(const std::array<uint64_t, 64>& bits);
    /// Returns kInvalid for a mesh with no usable triangle.
    MeshId addMesh(MeshSource source);
    void removeMesh(MeshId mesh);

    InstanceId addInstance(MeshId mesh, const glm::mat4& objectToWorld);
    /// An instance of the per-frame top level; `group` 0 for none. Its id has
    /// kDynamicBit set; the two calls below take either kind.
    InstanceId addDynamicInstance(MeshId mesh, const glm::mat4& objectToWorld, GroupId group = 0);
    void setInstanceTransform(InstanceId instance, const glm::mat4& objectToWorld);
    void removeInstance(InstanceId instance);
    [[nodiscard]] static bool isDynamic(InstanceId instance) {
        return instance != kInvalid && (instance & kDynamicBit) != 0;
    }
    /// Whether the mesh has made it into the pools (and has its BLAS), so an
    /// instance of it would be traced.
    [[nodiscard]] bool meshReady(MeshId mesh) const;

    /// Groups for addDynamicInstance, never 0. A group starts with no box,
    /// which nothing is inside.
    GroupId addGroup();
    void setGroupBounds(GroupId group, const glm::vec3& bmin, const glm::vec3& bmax);
    void removeGroup(GroupId group);

    /// Build and upload while active; release everything GPU-side when not.
    /// Switching off must happen with the device idle.
    void setActive(bool active);
    [[nodiscard]] bool isActive() const { return active_; }
    /// Every mesh still waiting to be built and uploaded, now, in submits of
    /// their own that are waited on - rather than a share a frame, which on
    /// entering a city was some four seconds of 100 ms frames (14,000
    /// meshes at 150,000 triangles a frame). For the loading screen, and for
    /// the frame the tracing is switched on.
    void uploadAllPending();

    /// Record uploads and acceleration structure builds for this frame.
    /// Must run outside a render pass and before any trace dispatch.
    void update(VkCommandBuffer cmd);

    [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const { return setLayout_; }
    /// The set for the current frame slot. Valid after update().
    [[nodiscard]] VkDescriptorSet descriptorSet() const;
    /// False until at least one instance has made it into an acceleration
    /// structure; tracing an empty scene is legal but pointless.
    [[nodiscard]] bool hasContent() const { return liveInstances_ + liveDynInstances_ > 0; }

    struct Stats {
        uint32_t meshes = 0;
        uint32_t instances = 0;
        uint32_t dynamicInstances = 0;
        uint64_t triangles = 0;
        uint64_t gpuBytes = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct Mesh {
        bool live = false;
        uint32_t triOffset = RtRangeAllocator::kFailed;
        uint32_t triCount = 0;
        uint32_t nodeOffset = RtRangeAllocator::kFailed;
        uint32_t nodeCount = 0;
        RtAabb bounds;
        bool uploaded = false;
        bool queued = false;
        bool prebuilt = false;  ///< built by uploadAllPending's workers, waiting for its upload
        // Static instances placing it: only their arrival in the pools
        // changes the static top level.
        uint32_t staticRefs = 0;
        MeshSource source;
        // The cutout triangles' UVs and masks (RtTriExtra in the shaders).
        uint32_t extraOffset = RtRangeAllocator::kFailed;
        uint32_t extraCount = 0;
        // Built from the source for one upload; cleared once copied.
        std::vector<RtTriangle> tris;
        std::vector<RtBvhNode> nodes;
        std::vector<glm::uvec4> extras;
        // Hardware backend.
        AllocatedBuffer blasBuffer{};
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        VkDeviceAddress blasAddress = 0;
        bool hasTranslucent = false;
    };
    struct Instance {
        bool live = false;
        MeshId mesh = kInvalid;
        glm::mat4 objectToWorld{1.0f};
        GroupId group = 0;  // dynamic instances only
    };
    struct Group {
        bool live = false;
        glm::vec4 bmin{1e30f};
        glm::vec4 bmax{-1e30f};
    };
    /// What a trace shader reads per mesh (std430).
    struct MeshGPU {
        uint32_t nodeOffset;
        uint32_t triOffset;
        uint32_t pad0;
        uint32_t pad1;
    };
    /// What a trace shader reads per instance (std430): the world-to-object
    /// transform as three rows, the mesh it places, that mesh's flags and the
    /// instance's group (0 for none).
    struct InstanceGPU {
        glm::vec4 worldToObject[3];
        uint32_t mesh;
        uint32_t flags;
        uint32_t group;
        uint32_t pad;
    };
    /// A hardware top level and what building it takes; one of each kind per
    /// frame slot.
    struct TlasResources {
        AllocatedBuffer asInstances{}; // VkAccelerationStructureInstanceKHR[], mapped
        uint32_t asInstanceCapacity = 0;
        AllocatedBuffer tlasBuffer{};
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        VkDeviceSize tlasSize = 0;
        AllocatedBuffer scratch{};
        VkDeviceSize scratchSize = 0;
    };

    struct GpuPool {
        AllocatedBuffer buffer{};
        RtRangeAllocator alloc;
        uint32_t elementSize = 0;
        VkBufferUsageFlags usage = 0;
    };
    struct FrameResources {
        AllocatedBuffer instances{};   // InstanceGPU[], mapped: the static ones, then the dynamic
        uint32_t instanceCapacity = 0;
        AllocatedBuffer tlasNodes{};   // RtBvhNode[], mapped (software); see kTopHeader
        uint32_t tlasNodeCapacity = 0;
        AllocatedBuffer groups{};      // two vec4 per group id, mapped
        uint32_t groupCapacity = 0;
        // Hardware top levels.
        TlasResources staticAs, dynamicAs;
        uint64_t uploadedVersion = ~0ull;
        uint64_t descriptorGeneration = ~0ull;
        uint32_t instanceCount = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool loadHardwareEntryPoints();
    bool createDescriptors();
    void ensurePoolCapacity(VkCommandBuffer cmd, GpuPool& pool, uint32_t needed);
    uint32_t poolAllocate(VkCommandBuffer cmd, GpuPool& pool, uint32_t count);
    void uploadPending(VkCommandBuffer cmd);
    void buildPendingBlas(VkCommandBuffer cmd, const std::vector<MeshId>& meshes);
    void uploadFrame(VkCommandBuffer cmd, FrameResources& fr);
    void buildStatic();
    void buildDynamic();
    void buildTlas(VkCommandBuffer cmd, TlasResources& as,
                   const std::vector<VkAccelerationStructureInstanceKHR>& data, bool fastBuild);
    void destroyTlas(TlasResources& as);
    void writeDescriptors(FrameResources& fr);
    void destroyMeshGpu(Mesh& m);
    void releaseGpu();
    void buildMesh(Mesh& m);
    VkDeviceAddress bufferAddress(VkBuffer buffer) const;
    /// Returns true when the buffer was replaced and holds nothing yet.
    bool ensureMapped(AllocatedBuffer& buf, uint32_t& capacity, uint32_t needed,
                      uint32_t elementSize, VkBufferUsageFlags usage);
    void stage(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset, const void* data,
               VkDeviceSize size);

    VkContext* ctx_ = nullptr;
    // Deferred releases check this before touching the pools.
    std::shared_ptr<bool> alive_;
    bool active_ = false;
    // Bumped by releaseGpu(); a deferred release from an older epoch refers
    // to pools that no longer exist.
    uint64_t gpuEpoch_ = 0;
    Backend backend_ = Backend::Software;

    std::vector<Mesh> meshes_;
    std::vector<MeshId> freeMeshes_;
    std::vector<MeshId> pendingMeshes_;
    std::vector<Instance> instances_;
    std::vector<InstanceId> freeInstances_;
    std::vector<Instance> dynInstances_;
    std::vector<InstanceId> freeDynInstances_;
    std::vector<Group> groups_;  // [0] is "no group" and stays empty
    std::vector<GroupId> freeGroups_;
    uint32_t liveMeshes_ = 0;
    uint32_t liveInstances_ = 0;
    uint32_t liveDynInstances_ = 0;
    uint64_t liveTriangles_ = 0;

    GpuPool triPool_;
    GpuPool nodePool_;
    GpuPool extraPool_;
    // Alpha masks, 64 uint64 each, and how many of them the GPU has.
    std::vector<uint64_t> masks_;
    std::unordered_map<uint64_t, uint32_t> maskByHash_;
    AllocatedBuffer maskBuffer_{};
    uint32_t maskCapacity_ = 0;
    uint32_t masksUploaded_ = 0;
    void uploadMasks(VkCommandBuffer cmd);
    AllocatedBuffer meshTable_{};
    uint32_t meshTableCapacity_ = 0;
    bool meshTableDirty_ = true;

    // Bumped whenever the instance set, a transform, or a mesh's placement in
    // the pools changes; each frame slot re-uploads when it lags behind.
    uint64_t sceneVersion_ = 0;
    // Bumped whenever a buffer a descriptor points at is replaced.
    uint64_t descriptorGeneration_ = 0;

    // CPU copies of the static per-frame data, built once per version. The
    // software top level's nodes are stored after kTopHeader and point there.
    uint64_t builtVersion_ = ~0ull;
    std::vector<InstanceGPU> instanceData_;
    std::vector<RtBvhNode> tlasNodes_;
    std::vector<VkAccelerationStructureInstanceKHR> asInstanceData_;
    RtAabb staticBounds_;
    // The same of the dynamic instances, built every update. Their rows
    // follow the static ones, their nodes the static nodes.
    std::vector<InstanceGPU> dynInstanceData_;
    std::vector<RtBvhNode> dynTlasNodes_;
    std::vector<VkAccelerationStructureInstanceKHR> dynAsInstanceData_;
    RtAabb dynamicBounds_;
    std::vector<glm::vec4> groupData_;

    FrameResources frames_[MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    // Bound where a real buffer does not exist yet, so every set is complete.
    AllocatedBuffer placeholder_{};

    VkDeviceSize scratchAlignment_ = 256;
    PFN_vkGetBufferDeviceAddress pfnGetBufferDeviceAddress_ = nullptr;
    PFN_vkCreateAccelerationStructureKHR pfnCreateAs_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR pfnDestroyAs_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetAsBuildSizes_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuildAs_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAsAddress_ = nullptr;
};

}  // namespace wowee::rendering
