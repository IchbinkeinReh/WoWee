#pragma once

#include "rendering/collision_geometry.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/spatial_grid.hpp"
#include "rendering/shadow_params.hpp"

#include "pipeline/blp_loader.hpp"
#include "pipeline/grass_clearing.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_doodad_light.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <glm/ext/vector_uint4_sized.hpp>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <vector>
#include <string>
#include <optional>
#include <limits>
#include <future>
#include <algorithm>

namespace wowee {
namespace pipeline {
    struct WMOModel;
    struct WMOGroup;
    class AssetManager;
}

namespace rendering {

class Camera;
class Frustum;
class M2Renderer;
class RtScene;
class VkContext;
class VkTexture;

/**
 * WMO (World Model Object) Renderer (Vulkan)
 *
 * Renders buildings, dungeons, and large structures from WMO files.
 * Features:
 * - Multi-material rendering
 * - Batched rendering per group
 * - Frustum culling
 * - Portal visibility (future)
 */
class WMORenderer {
public:
    WMORenderer();
    ~WMORenderer();

    /**
     * Initialize renderer (Vulkan)
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for set 0 (per-frame UBO)
     * @param assetManager Asset manager for loading textures (optional)
     */
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assetManager = nullptr);

    /**
     * Cleanup GPU resources
     */
    void shutdown();

    /**
     * Set M2 renderer for hierarchical transform updates (doodads follow parent WMO)
     */
    void setM2Renderer(M2Renderer* renderer) { m2Renderer_ = renderer; }

    /// Where loaded models register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }
    /// Bring the scene's instances in line with this renderer's: added,
    /// moved, hidden and removed. Once a frame, and only while the lighting
    /// is on; one pass over the instances is cheaper than hooking every one
    /// of the half-dozen ways they change.
    void syncRtScene();

    /**
     * Load WMO model and create GPU resources
     * @param model WMO model with geometry data
     * @param id Unique identifier for this WMO instance
     * @return True if successful
     */
    bool loadModel(const pipeline::WMOModel& model, uint32_t id);

    enum class ModelLoadResult { Complete, InProgress, Failed };

    /// Upload a model a few groups at a time. InProgress means call again with
    /// the same arguments; a single large model can otherwise take over a
    /// hundred milliseconds, which lands as a visible hitch when terrain
    /// streaming finalises a tile.
    ModelLoadResult loadModelIncremental(const pipeline::WMOModel& model, uint32_t id, float budgetMs);

    /**
     * Check if a WMO model is currently resident in the renderer
     * @param id WMO model identifier
     */
    bool isModelLoaded(uint32_t id) const;
    /// Whether this instance's model has finished uploading its groups, and so
    /// whether a failed floor query means "nothing under you" rather than "not
    /// loaded yet". The two need telling apart: a rider held in place waiting
    /// for collision that is already there waits forever.
    bool instanceHasCollisionGeometry(uint32_t instanceId) const;

    /**
     * Check if a WMO instance is still live in the renderer. Owners that cache
     * instance IDs use this to detect an instance dropped underneath them
     * (e.g. by a renderer-wide clear) instead of addressing a dead handle.
     * @param instanceId Instance identifier returned by createInstance
     */
    bool hasInstance(uint32_t instanceId) const;

    /**
     * Unload WMO model and free GPU resources
     * @param id WMO model identifier
     */
    void unloadModel(uint32_t id);

    /**
     * Create a WMO instance in the world
     * @param modelId WMO model to instantiate
     * @param position World position
     * @param rotation Rotation (euler angles in radians)
     * @param scale Uniform scale
     * @return Instance ID
     */
    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                           const glm::vec3& rotation = glm::vec3(0.0f),
                           float scale = 1.0f);

    /**
     * Update the world position of an existing instance (e.g., for transports)
     * @param instanceId Instance to update
     * @param position New world position
     */
    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);

    /**
     * Update the full transform of an existing instance (for moving transports)
     * @param instanceId Instance to update
     * @param transform World transform matrix
     */
    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);

    /// Mark an instance as a moving transport. Its collision still answers the
    /// static-world floor query (you walk onto a hull, you stand on a lift), but
    /// only when the deck is genuinely underfoot - see getFloorHeight. Idempotent;
    /// safe on every register.
    void setInstanceIsTransport(uint32_t instanceId, bool isTransport);

    /// Take an instance out of the world without unloading it.
    ///
    /// A cross-continent transport is one object on a route that spans two
    /// maps: for half its cycle the Undercity zeppelin is over Howling Fjord
    /// and is not here at all. Its hull holds still on this map's slice while
    /// it is away, so without this it would sit invisible-but-solid over the
    /// tower, and a player could stand on a zeppelin that is on another
    /// continent. Hidden instances are skipped by drawing and by every spatial
    /// query, which is where collision comes from.
    void setInstanceHidden(uint32_t instanceId, bool hidden);
    /// The game object highlight (0x00743c70 records the object's GUID; the
    /// WMO whose GUID it is gets the light's ambient added into c29 by
    /// 0x007964a0 / 0x007a8430). One at a time: clear, then set.
    void setInstanceHighlight(uint32_t instanceId, bool on);
    void clearInstanceHighlights();

    /**
     * Add doodad (child M2) to WMO instance
     * @param instanceId WMO instance to add doodad to
     * @param m2InstanceId M2 instance ID of the doodad
     * @param localTransform Local transform relative to WMO origin
     */
    void addDoodadToInstance(uint32_t instanceId, uint32_t m2InstanceId, const glm::mat4& localTransform);

    // Forward declare DoodadTemplate for public API
    struct DoodadTemplate {
        std::string m2Path;
        glm::mat4 localTransform;
        // A doodad of an interior group, lit by its MODD colour
        // (pipeline::wmo_doodad_light).
        bool interiorLit = false;
        glm::vec3 interiorAmbient{0.0f};
        glm::vec3 interiorDirect{0.0f};
    };

    /**
     * Get doodad templates for a WMO model
     * @param modelId WMO model ID
     * @return Vector of doodad templates (empty if no doodads or model not found)
     */
    /// Set an animation on every child doodad of an instance. Returns how many
    /// doodads were addressed, so a caller streaming them in over several frames
    /// can tell when the set has grown and re-apply. A doodad lacking the
    /// sequence is left as it is.
    size_t setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId, bool loop);
    const std::vector<DoodadTemplate>* getDoodadTemplates(uint32_t modelId) const;

    /**
     * Remove WMO instance
     * @param instanceId Instance to remove
     */
    void removeInstance(uint32_t instanceId);
    /**
     * Remove multiple WMO instances with a single spatial-index rebuild.
     */
    void removeInstances(const std::vector<uint32_t>& instanceIds);

    /**
     * Remove all instances
     */
    void clearInstances();

    /**
     * Clear all instances, loaded models, and texture cache (for map transitions)
     */
    void clearAll();

    /**
     * Render all WMO instances (Vulkan)
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0)
     * @param camera Camera for frustum culling
     */
    /** Pre-update mutable state (frame ID, material UBOs) on main thread before parallel render. */
    void prepareRender();
    /// viewerPos is the character; portal culling seeds from it as well as from
    /// the camera, because at a doorway the two stand in different groups and
    /// neither alone is reliably the right place to start.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                const glm::vec3* viewerPos = nullptr);

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);

    /**
     * Render depth-only for shadow casting
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f);

    /**
     * Get number of loaded models
     */
    void recreatePipelines();
    /// The four main-pass pipelines, which initialize() and
    /// recreatePipelines() both need and each used to describe.
    bool buildMainPassPipelines(VkDevice device,
                                wowee::rendering::VkShaderModule& vertShader,
                                wowee::rendering::VkShaderModule& fragShader);
    bool isInitialized() const { return initialized_; }
    uint32_t getModelCount() const { return loadedModels.size(); }

    /**
     * Get number of active instances
     */
    uint32_t getInstanceCount() const { return instances.size(); }
    size_t getLoadedModelCount() const { return loadedModels.size(); }

    /**
     * Remove models that have no instances referencing them
     * Call periodically to free GPU memory
     */
    void cleanupUnusedModels();

    /**
     * Get total triangle count (all instances)
     */
    uint32_t getTotalTriangleCount() const;

    /**
     * Get total draw call count (last frame)
     */
    uint32_t getDrawCallCount() const { return lastDrawCalls; }

    /**
     * Normal mapping / Parallax Occlusion Mapping settings
     */
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; materialSettingsDirty_ = true; }
    void setNormalMapStrength(float s) { normalMapStrength_ = s; materialSettingsDirty_ = true; }
    void setPOMEnabled(bool enabled) { pomEnabled_ = enabled; materialSettingsDirty_ = true; }
    void setPOMQuality(int q) { pomQuality_ = q; materialSettingsDirty_ = true; }
    bool isNormalMappingEnabled() const { return normalMappingEnabled_; }
    float getNormalMapStrength() const { return normalMapStrength_; }
    bool isPOMEnabled() const { return pomEnabled_; }
    int getPOMQuality() const { return pomQuality_; }

    /**
     * Enable/disable wireframe rendering
     */
    void setWireframeMode(bool enabled) { wireframeMode = enabled; }

    /**
     * Enable/disable frustum culling
     */
    void setFrustumCulling(bool enabled) { frustumCulling = enabled; }

    /**
     * Enable/disable portal-based visibility culling
     */
    void setPortalCulling(bool enabled) { portalCulling = enabled; }
    bool isPortalCullingEnabled() const { return portalCulling; }

    /**
     * Enable/disable distance-based group culling
     */
    void setDistanceCulling(bool enabled, float maxDistance = 500.0f) {
        distanceCulling = enabled;
        maxGroupDistance = maxDistance;
        maxGroupDistanceSq = maxDistance * maxDistance;
    }
    bool isDistanceCullingEnabled() const { return distanceCulling; }
    float getMaxGroupDistance() const { return maxGroupDistance; }
    void setViewDistance(float distance) { viewDistance_ = std::clamp(distance, 400.0f, 2400.0f); }

    /**
     * Get number of groups culled by portals last frame
     */
    uint32_t getPortalCulledGroups() const { return lastPortalCulledGroups; }

    /**
     * Get number of groups culled by distance last frame
     */
    uint32_t getDistanceCulledGroups() const { return lastDistanceCulledGroups; }

    /**
     * Enable/disable GPU occlusion query culling (stubbed in Vulkan)
     */
    void setOcclusionCulling(bool /*enabled*/) { /* stubbed */ }
    bool isOcclusionCullingEnabled() const { return false; }

    /**
     * Get number of groups culled by occlusion queries last frame
     */
    uint32_t getOcclusionCulledGroups() const { return 0; }

    // Lighting/fog/shadow are now in the per-frame UBO; these are no-ops for API compat
    void setFog(const glm::vec3& /*color*/, float /*start*/, float /*end*/) {}
    void setLighting(const float /*lightDir*/[3], const float /*lightColor*/[3],
                     const float /*ambientColor*/[3]) {}
    void setShadowMap(uint32_t /*depthTex*/, const glm::mat4& /*lightSpace*/) {}
    void clearShadowMap() {}

    /**
     * Get floor height at a GL position via ray-triangle intersection.
     * @param outNormalZ If not null, receives the Z component of the floor surface normal
     *                   (1.0 = flat, 0.0 = vertical). Useful for slope walkability checks.
     */
    /// The floor under (glX, glY), searching at or below glZ. By default it
    /// returns the highest such floor, which is the step-up behaviour. Pass
    /// `referenceZ` (the querier's actual feet height) to instead get the floor
    /// closest to the feet - which is what keeps a player standing on the lower
    /// of two stacked floors under an overhang, rather than being snapped up to
    /// the level above them.
    std::optional<float> getFloorHeight(float glX, float glY, float glZ,
                                        float* outNormalZ = nullptr,
                                        float referenceZ =
                                            std::numeric_limits<float>::quiet_NaN()) const;

    /** Query floor collision from one moving WMO instance. */
    std::optional<float> getInstanceFloorHeight(uint32_t instanceId,
                                                float glX, float glY, float glZ,
                                                float* outNormalZ = nullptr) const;

    /** Dump diagnostic info about WMO groups overlapping a position */
    void debugDumpGroupsAtPosition(float glX, float glY, float glZ) const;

    /**
     * Append the 2D footprints of instances touching the given window, for
     * the grass generator's clearings - grass grows short and sparse beside
     * a building the way it does beside a road. Per-group boxes rather than
     * the instance's outer bounds: a keep's outer box swallows its own
     * courtyards, and a courtyard is exactly where grass belongs.
     */
    void collectGrassClearings(float minX, float minY, float maxX, float maxY,
                               std::vector<pipeline::GrassClearingSource>& out) const;

    /**
     * Check wall collision and adjust position
     * @param from Starting position
     * @param to Desired position
     * @param adjustedPos Output adjusted position (pushed away from walls)
     * @param insideWMO If true, use tighter collision for indoor precision
     * @return true if collision occurred
     */
    bool checkWallCollision(const glm::vec3& from, const glm::vec3& to, glm::vec3& adjustedPos, bool insideWMO = false) const;

    /// Whether solid WMO geometry stands between two points - a sight line
    /// rather than a step, so no player radius and no step height. See the
    /// definition for why checkWallCollision cannot answer this.
    bool segmentBlocked(const glm::vec3& from, const glm::vec3& to) const;

    /**
     * Check if a position is inside any WMO
     * @param outModelId If not null, receives the model ID of the WMO
     * @return true if inside a WMO
     */
    bool isInsideWMO(float glX, float glY, float glZ, uint32_t* outModelId = nullptr) const;

    /**
     * Check if a position is inside an interior WMO group (flag 0x2000).
     * Used to dim M2 lighting for doodads placed indoors.
     */
    bool isInsideInteriorWMO(float glX, float glY, float glZ) const;

    /// The fog of the WMO interior the camera is in, as 0x007a1150 finds it:
    /// the first group around `pos` that is neither exterior nor exterior-lit
    /// (flags 0x48), the model's fog 0 lerped toward each of that group's fog
    /// spheres the camera is within, farthest first, and how far the camera
    /// is from the way out - the client walks portals to an exterior group
    /// (0x007d8010); this is the straight distance to the nearest portal of
    /// the group that opens onto one.
    struct InteriorFog {
        float end = 0.0f;           ///< yards
        float startScalar = 0.0f;   ///< start as a fraction of end
        glm::vec3 color{0.0f};
        float liquidEnd = 0.0f;     ///< the second fog, for the camera in liquid
        float liquidStartScalar = 0.0f;
        glm::vec3 liquidColor{0.0f};
        uint32_t flags = 0;         ///< MFOG flags
        float distanceInside = 0.0f;
    };
    [[nodiscard]] std::optional<InteriorFog> interiorFogAt(const glm::vec3& pos) const;

    /// The WMO floor a world object's floor search meets (0x007c28f0's WMO
    /// half, 0x007c2700): the first triangle straight from `start` to
    /// `endZ`, across every WMO there, passing over groups with 0x80,
    /// 0x10000 or 0x400000. `light` is what the object is lit by when that
    /// is an interior group's floor with vertex colours
    /// (pipeline::wmo_doodad_light::unitLight of the colour there, 0x007c7fe0),
    /// none on an exterior or exterior-lit group's floor (0x007c15f0) - the
    /// zone's light then. On a transition face the floor's alpha says how far
    /// toward the outside's light it is (FloorLight::towardOutside).
    struct ObjectFloor {
        float z = 0.0f;
        std::optional<pipeline::wmo_doodad_light::FloorLight> light;
    };
    [[nodiscard]] std::optional<ObjectFloor> floorAlong(const glm::vec3& start, float endZ) const;

    /// Works out which groups are drawn in the camera's interior pass
    /// (pipeline::wmo_doodad_light::interiorPassGroups): a walk through the
    /// portals from the groups the camera is in, through those it can see
    /// (`frustum`, or every portal when null). Once a frame, before the lights
    /// and the draw.
    void updateInteriorPass(const glm::vec3& cameraPos, const Frustum* frustum);
    /// Whether a point is in a group drawn in that pass: what sets an
    /// object's 0x8000, and the camera's fog colour with it (0x007c1730).
    [[nodiscard]] bool inInteriorPass(const glm::vec3& pos) const;
    /// Changes whenever the set of those groups does.
    [[nodiscard]] uint64_t interiorPassGeneration() const { return interiorPassGeneration_; }

    /**
     * Raycast against WMO bounding boxes for camera collision
     * @param origin Ray origin (e.g., character head position)
     * @param direction Ray direction (normalized)
     * @param maxDistance Maximum ray distance to check
     * @return Distance to first intersection, or maxDistance if no hit
     */
    float raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;

    /**
     * Limit expensive collision/raycast queries to objects near a focus point.
     */
    void setCollisionFocus(const glm::vec3& worldPos, float radius);

    void resetQueryStats();
    double getQueryTimeMs() const { return queryTimeMs; }
    uint32_t getQueryCallCount() const { return queryCallCount; }

    /**
     * Update the tracked active WMO group based on player position.
     * Called at low frequency (every ~10 frames or on significant movement).
     */
    void updateActiveGroup(float glX, float glY, float glZ);

    // Floor cache persistence (zone-specific files)
    void setMapName(const std::string& name) { mapName_ = name; }
    const std::string& getMapName() const { return mapName_; }
    bool saveFloorCache() const;  // Saves to cache/wmo_floor_<mapName>.bin
    bool loadFloorCache();        // Loads from cache/wmo_floor_<mapName>.bin
    size_t getFloorCacheSize() const { return precomputedFloorGrid.size(); }

    // Pre-compute floor cache for all loaded WMO instances
    void precomputeFloorCache();

    // Pre-decoded BLP cache: set before calling loadModel() to skip main-thread BLP decode
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

    // Normal/height pixels may be generated by terrain workers alongside the
    // decoded diffuse textures. loadTexture() then only performs the bounded GPU
    // upload while building the WMO model on the main thread.
    void setPredecodedNormalMapCache(
        std::unordered_map<std::string, pipeline::BLPImage>* cache,
        std::unordered_map<std::string, float>* variances) {
        predecodedNormalMapCache_ = cache;
        predecodedNormalMapVariances_ = variances;
    }
    static pipeline::BLPImage generateNormalHeightMapPixels(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance);

    // Defer normal/height map generation during streaming to avoid CPU stalls
    void setDeferNormalMaps(bool defer) { deferNormalMaps_ = defer; }

private:
    // WMO material UBO - matches WMOMaterial in wmo.frag.glsl
    struct WMOMaterialUBO {
        int32_t hasTexture;        // 0
        int32_t alphaTest;         // 4
        int32_t outsideLight;      // 8: a transition batch's first pass, 0x007a8b10's mode (BatchLight)
        int32_t insideLight;       // 12: its second pass, or the only one
        float specularIntensity;   // 16
        int32_t transition;        // 20: a transition batch, its two passes blended by the output alpha
        int32_t enableNormalMap;   // 24
        int32_t enablePOM;         // 28
        float pomScale;            // 32 (height scale)
        int32_t pomMaxSamples;     // 36 (max ray-march steps)
        float heightMapVariance;   // 40 (low variance = skip POM)
        float normalMapStrength;   // 44 (0=flat, 1=full, 2=exaggerated)
        int32_t fogs;              // 48: per pass, bits 0-1 the first and 2-3 the second: 0 none, 1 the zone's, 2 the group's inside fog
        float wmoAmbientR;         // 52 (interior ambient color R)
        float wmoAmbientG;         // 56 (interior ambient color G)
        float wmoAmbientB;         // 60 (interior ambient color B)
        int32_t unifiedPath;        // 64: MOHD flag 0x2, the MapObjU* programs (0x007a9380)
        int32_t hasVertexColors;    // 68: 0 no MOCV, 1 as the MOCV has them (MOHD 0x8), 2 as 0x007d7380 left them
        int32_t shadowed;           // 72: bit 0 the first pass samples the sun's shadow map, bit 1 the second
        int32_t program;            // 76: the pixel program (wmo_doodad_light::SurfaceProgram)
        float alphaRef;             // 80: the alpha test's reference (0x00ad8b7c), 0 for none
        float sidnR;                // 84: MOMT sidnColor where flag 0x10, else 0 (wmo_sidn.hpp)
        float sidnG;                // 88
        float sidnB;                // 92
    };  // 96 bytes total

    /// What a group's draw pushes to wmo.frag after the model matrix:
    /// interiorPass 1 when the group is drawn in the camera's interior pass
    /// (wmo_doodad_light::interiorPassGroups), whose interior batches the
    /// client fogs in the camera's colour (0x007a9380).
    struct WMOGroupPush {
        int32_t interiorPass = 0;
        int32_t highlight = 0;  ///< the light's ambient added into c29
        int32_t pad[2] = {};
    };

    /**
     * WMO group GPU resources
     */
    struct GroupResources {
        ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        ::VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;

        uint32_t groupFlags = 0;
        bool allUntextured = false;  // True if ALL batches use fallback white texture (collision/placeholder group)
        bool isLOD = false;          // Distance-only group (skip when camera is close)

        // Material batches (start index, count, material ID)
        struct Batch {
            uint32_t startIndex;   // First index in EBO
            uint32_t indexCount;   // Number of indices to draw
            uint8_t materialId;    // Material/texture reference
            /// One of MOGP's first transBatchCount batches, drawn as a blend
            /// of the outside and inside light (0x007a9380).
            bool transition = false;
            /// One of the intBatchCount that follow them (0x007ac9f0).
            bool interior = false;
        };
        std::vector<Batch> batches;

        // Pre-merged batches for efficient rendering (computed at load time)
        struct MergedBatch {
            VkTexture* texture = nullptr;   // from cache, NOT owned
            VkTexture* normalHeightMap = nullptr;  // generated from diffuse, NOT owned
            float heightMapVariance = 0.0f; // variance of height map (low = flat texture)
            VkDescriptorSet materialSet = VK_NULL_HANDLE;  // set 1
            ::VkBuffer materialUBO = VK_NULL_HANDLE;
            VmaAllocation materialUBOAlloc = VK_NULL_HANDLE;
            bool hasTexture = false;
            bool alphaTest = false;
            bool unlit = false;
            bool isTransparent = false;     // blendMode >= 2
            bool transition = false;        // Batch::transition
            bool unfogged = false;          // F_UNFOGGED (0x2), read only on a transition batch
            bool averaged = false;          // material flag 0x20 where 0x007a9380 reads it: light mode 2
            VkTexture* envTexture = nullptr;  // MOMT texture_2 for MapObjEnv/EnvMetal, NOT owned
            uint32_t surfaceKey = 0;        // the lights, fogs, shadows and program packed (WMOMaterialUBO)
            uint32_t sidn = 0;              // MOMT sidnColor where flag 0x10 (WMOMaterialUBO)
            // For multi-draw: store index ranges
            struct DrawRange { uint32_t firstIndex; uint32_t indexCount; };
            std::vector<DrawRange> draws;
        };
        std::vector<MergedBatch> mergedBatches;

        // Collision geometry (positions only, for floor raycasting)
        std::vector<glm::vec3> collisionVertices;
        std::vector<uint16_t> collisionIndices;

        // 2D spatial grid for fast triangle lookup (built at load time).
        // Bins triangles by their XY bounding box into grid cells.
        static constexpr float COLLISION_CELL_SIZE = 4.0f;
        int gridCellsX = 0;
        int gridCellsY = 0;
        glm::vec2 gridOrigin;  // XY of bounding box min
        // cellTriangles[cellY * gridCellsX + cellX] = list of triangle start indices
        std::vector<std::vector<uint32_t>> cellTriangles;

        // Pre-classified triangle lists per cell (built at load time)
        std::vector<std::vector<uint32_t>> cellFloorTriangles;  // abs(normal.z) >= 0.35
        std::vector<std::vector<uint32_t>> cellWallTriangles;   // abs(normal.z) < 0.35

        // Pre-computed per-triangle Z bounds for fast vertical reject
        struct TriBounds { float minZ; float maxZ; };
        std::vector<TriBounds> triBounds;  // indexed by triStart/3

        // Pre-computed per-triangle normals (unit length, indexed by triStart/3)
        std::vector<glm::vec3> triNormals;

        // Per-collision-triangle MOPY flags (indexed by collision tri index, i.e. triStart/3)
        std::vector<uint8_t> triMopyFlags;
        /// An interior group's vertex colours (MOCV, rgba 0..255) as the
        /// client keeps them once loaded (wmo_doodad_light::loadedVertexColor),
        /// one per collision vertex, for the light a unit standing on it
        /// takes (floorAlong). Empty for the other groups.
        std::vector<glm::u8vec4> collisionColors;
        /// True when no triangle in this group blocks: no collision hull, and
        /// nothing rendered that is not detail. Detail never blocks, so such a
        /// group is walk-through in its entirety - which is a thing to be
        /// walked through only if it was meant to be, and Darkshore's bridges
        /// are 428 triangles of it.
        bool noBlockingTriangles = false;

        // Scratch bitset for deduplicating triangle queries (sized to numTriangles)
        mutable std::vector<uint8_t> triVisited;

        // Build the spatial grid from collision geometry
        void buildCollisionGrid();


        // Get triangle indices for a local-space XY range (for wall collision)
        /// The triangles of one of the three cell arrays that a query box
        /// reaches, deduplicated. The three queries below differ only in
        /// which array they pass.
        void gatherCellTriangles(const std::vector<std::vector<uint32_t>>& cells,
                                 float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;

        void getTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;

        // Get pre-classified floor/wall triangles in range
        void getFloorTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                      std::vector<uint32_t>& out) const;
        void getWallTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                     std::vector<uint32_t>& out) const;
    };

    /**
     * Portal data for visibility culling
     */
    struct PortalData {
        uint16_t startVertex;
        uint16_t vertexCount;
        glm::vec3 normal;
        float distance;
    };

    struct PortalRef {
        uint16_t portalIndex;
        uint16_t groupIndex;
        int16_t side;
    };

    /**
     * Loaded WMO model data
     */
    struct ModelData {
        uint32_t id;
        std::vector<GroupResources> groups;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;
        glm::vec3 wmoAmbientColor{0.5f, 0.5f, 0.5f};  // From MOHD, used for interior lighting
        uint32_t mohdFlags = 0;                        // MOHD flags (0x2: see floorAlong)
        bool isLowPlatform = false;

        // Doodad templates (M2 models placed in WMO, stored for instancing)
        // Uses the public DoodadTemplate struct defined above
        std::vector<DoodadTemplate> doodadTemplates;

        // Texture handles for this model (indexed by texture path order)
        std::vector<VkTexture*> textures;  // non-owning, from cache
        std::vector<std::string> textureNames;  // lowercase texture paths (parallel to textures)

        // Material texture indices (materialId -> texture index)
        std::vector<uint32_t> materialTextureIndices;

        // Material blend modes (materialId -> blendMode; 1 = alpha-test cutout)
        std::vector<uint32_t> materialBlendModes;

        // Material flags (materialId -> flags; 0x01 = unlit)
        std::vector<uint32_t> materialFlags;

        // MOMT's shader (materialId -> the effect table's index, 0x007afee0)
        std::vector<uint32_t> materialShaders;
        /// MOMT sidnColor (+0x10) where the material has flag 0x10, else 0:
        /// what its windows are lit by at night (wmo_sidn.hpp).
        std::vector<uint32_t> materialSidn;

        // MOMT's texture_2 (materialId -> texture index, ~0u for none): the
        // map MapObjEnv and MapObjEnvMetal reflect.
        std::vector<uint32_t> materialEnvTextureIndices;

        // Portal visibility data
        std::vector<PortalData> portals;
        std::vector<glm::vec3> portalVertices;
        std::vector<PortalRef> portalRefs;
        // For each group: which portal refs belong to it (start index, count)
        std::vector<std::pair<uint16_t, uint16_t>> groupPortalRefs;
        // MFOG, and each group's four indices into it (0 = none)
        std::vector<pipeline::WMOFog> fogs;
        std::vector<std::array<uint8_t, 4>> groupFogIndices;

        // Set once the textures and materials below have been populated, so a
        // resumed load skips straight to the groups it has left.
        bool setupDone = false;
        // Next group to upload. A large model's groups are spread across several
        // calls under a time budget rather than uploaded in one stall.
        // Next texture to upload. Uploading them all at once cost 40ms on a
        // transport - the images are large, and the expense is the GPU upload
        // rather than the decode, which the worker already did.
        size_t nextTextureIndex = 0;
        size_t nextGroupIndex = 0;
        uint32_t loadedGroups = 0;

        [[nodiscard]] uint32_t getTotalTriangles() const {
            uint32_t total = 0;
            for (const auto& group : groups) {
                total += group.indexCount / 3;
            }
            return total;
        }
    };

    /**
     * WMO instance in the world
     */
    struct WMOInstance {
        uint32_t id;
        uint32_t modelId;
        glm::vec3 position;
        glm::vec3 rotation;  // Euler angles (radians)
        float scale;
        glm::mat4 modelMatrix;
        glm::mat4 invModelMatrix;  // Cached inverse for collision
        glm::vec3 worldBoundsMin;
        glm::vec3 worldBoundsMax;
        std::vector<std::pair<glm::vec3, glm::vec3>> worldGroupBounds;

        // Doodad tracking: M2 instances that are children of this WMO
        struct DoodadInfo {
            uint32_t m2InstanceId;       // ID of the M2 instance
            glm::mat4 localTransform;    // Local transform relative to WMO origin
        };
        std::vector<DoodadInfo> doodads;

        // A moving transport (ship hull, elevator). It keeps ordinary collision -
        // a rider gets exact deck height from getInstanceFloorHeight, but everyone
        // else needs the hull solid to walk aboard in the first place. The flag
        // only restricts how far from the feet the static floor query will accept
        // this deck, so a lift cycling past a bystander cannot become their ground
        // (the Undercity elevator yo-yo).
        bool isTransport = false;

        /// Loaded, positioned, and not in the world right now. See
        /// setInstanceHidden.
        bool hidden = false;
        /// The game object highlight (setInstanceHighlight).
        bool highlighted = false;

        void updateModelMatrix();
    };

    /// Recomputes one instance's world and per-group bounds after its
    /// model matrix has changed.
    void refreshInstanceBounds(WMOInstance& inst);

    /// Whether a collision focus is set and this instance is outside it.
    bool outsideCollisionFocus(const WMOInstance& instance) const;

    /// Whether a point is inside any of a WMO's groups, or only its
    /// interior ones. The two public queries above differ by that flag.
    bool isInsideWMOGroups(float glX, float glY, float glZ,
                           bool interiorOnly, uint32_t* outModelId) const;

    /// Whether a point is inside an instance's world bounds, with the Z
    /// window widened by the caller's margins.
    bool withinWorldBounds(const WMOInstance& instance,
                           float glX, float glY, float glZ,
                           float zMarginDown = 0.0f, float zMarginUp = 0.0f) const;

    /**
     * Create GPU resources for a WMO group
     */
    bool createGroupResources(const pipeline::WMOGroup& group, GroupResources& resources, uint32_t groupFlags = 0,
                              uint32_t mohdFlags = 0);

    /**
     * Check if group is visible in frustum
     */
    bool isGroupVisible(const GroupResources& group, const glm::mat4& modelMatrix,
                       const Camera& camera) const;

    /**
     * Find which group index contains a position (model space)
     * @return Group index or -1 if outside all groups
     */
    int findContainingGroup(const ModelData& model, const glm::vec3& localPos) const;

    /**
     * Get visible groups via portal traversal
     */
    /// Seeds from both the camera's group and the character's: at a doorway the
    /// two are in different rooms, and walking from only one while judging every
    /// door against the camera's frustum empties the interior.
    void getVisibleGroupsViaPortals(const ModelData& model,
                                     const glm::vec3& cameraLocalPos,
                                     const glm::vec3& viewerLocalPos,
                                     const Frustum& frustum,
                                     const glm::mat4& modelMatrix,
                                     std::unordered_set<uint32_t>& outVisibleGroups) const;

    /**
     * Test if a portal polygon is visible from a position through a frustum
     */
    bool isPortalVisible(const ModelData& model, uint16_t portalIndex,
                         const glm::vec3& cameraLocalPos,
                         const Frustum& frustum,
                         const glm::mat4& modelMatrix) const;

    /**
     * Load a texture from path
     */
    VkTexture* loadTexture(const std::string& path);
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedNormalMapCache_ = nullptr;
    std::unordered_map<std::string, float>* predecodedNormalMapVariances_ = nullptr;

    /**
     * Generate normal+height map from diffuse RGBA8 pixels
     * @param pixels RGBA8 pixel data
     * @param width Texture width
     * @param height Texture height
     * @param outVariance Receives height map variance (for POM threshold)
     * @return Generated VkTexture (RGBA8: RGB=normal, A=height)
     */
    std::unique_ptr<VkTexture> generateNormalHeightMap(const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance);

    /**
     * Allocate a material descriptor set from the pool
     */
    VkDescriptorSet allocateMaterialSet();

    /**
     * Destroy GPU resources for a single group.
     * When defer=true, destruction is scheduled via deferAfterFrameFence
     * so in-flight command buffers are not invalidated.
     */
    void destroyGroupGPU(GroupResources& group, bool defer = false);


    void rebuildSpatialIndex();
    void gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<size_t>& outIndices) const;

    // Vulkan context
    VkContext* vkCtx_ = nullptr;

    // Asset manager for loading textures
    pipeline::AssetManager* assetManager = nullptr;

    // M2 renderer for hierarchical transforms (doodads following WMO parent)
    M2Renderer* m2Renderer_ = nullptr;

    RtScene* rtScene_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> rtModelMeshes_;  // modelId -> RtScene mesh
    struct RtInstanceRecord {
        uint32_t rtId;
        uint32_t modelId;
        glm::mat4 matrix;
        uint64_t seen;
    };
    std::unordered_map<uint32_t, RtInstanceRecord> rtInstances_;  // WMO instance id ->
    uint64_t rtSyncGeneration_ = 0;
    void registerRtModel(uint32_t modelId, const ModelData& model);
    void releaseRtModel(uint32_t modelId);

    // Current map name for zone-specific floor cache
    std::string mapName_;

    // Vulkan pipelines
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;
    VkPipeline wireframePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering (Phase 7)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    // Descriptor set layouts
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;

    // Descriptor pool for material sets
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_MATERIAL_SETS = 32768;

    // Texture cache (path -> VkTexture)
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        std::unique_ptr<VkTexture> normalHeightMap;  // generated normal+height from diffuse
        float heightMapVariance = 0.0f;  // variance of generated height map
        bool alphaChannel = false;       // the BLP has alpha bits (0x004b54f0 reads the inverse)
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 8192ull * 1024 * 1024;  // 8 GB default, overridden at init
    std::unordered_set<std::string> failedTextureCache_;
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint64_t textureLookupSerial_ = 0;
    uint32_t textureBudgetRejectWarnings_ = 0;

    // Default white texture
    std::unique_ptr<VkTexture> whiteTexture_;

    // Flat normal placeholder (128,128,255,128) = up-pointing normal, mid-height
    std::unique_ptr<VkTexture> flatNormalTexture_;

    // Loaded models (modelId -> ModelData)
    std::unordered_map<uint32_t, ModelData> loadedModels;
    // Models part-way through an incremental load; moved into loadedModels once
    // every group is uploaded, so a half-built model is never rendered.
    std::unordered_map<uint32_t, ModelData> loadingModels_;
    size_t modelCacheLimit_ = 4000;
    uint32_t modelLimitRejectWarnings_ = 0;

    // Active instances
    std::vector<WMOInstance> instances;
    uint32_t nextInstanceId = 1;

    bool initialized_ = false;

    // Normal mapping / POM settings
    bool normalMappingEnabled_ = false;  // off by default: the client has none
    bool deferNormalMaps_ = false;       // skip normal map gen during streaming
    float normalMapStrength_ = 0.8f;     // 0.0 = flat, 1.0 = full, 2.0 = exaggerated
    bool pomEnabled_ = false;            // off by default: the client has none
    int pomQuality_ = 1;                 // 0=Low(16), 1=Medium(32), 2=High(64)
    bool materialSettingsDirty_ = false; // rebuild UBOs when settings change

    // Rendering state
    bool wireframeMode = false;
    bool frustumCulling = true;  // dead: every instance is included, the group cull does the work
    bool portalCulling = true;   // AABB transform bug fixed; conservative frustum test (no plane-side check) is visually safe
    bool distanceCulling = false;  // Disabled - causes ground to disappear

    /// Master switch, and it is off: no WMO group is dropped for any reason.
    ///
    /// Buildings kept disappearing from angles that had no business hiding
    /// them. Distance culling was turned off years ago for the same complaint
    /// ("causes ground to disappear") and it did not settle the matter, because
    /// the distance test below runs whether that flag is set or not - the flag
    /// only chooses which of two distances to use, so everything past
    /// viewDistance_ vanished regardless.
    ///
    /// The set of instances is already bounded by which tiles are loaded, so
    /// what this costs is the groups of loaded buildings that are behind the
    /// camera or too far to matter, not an unbounded scene.
    ///
    /// WOWEE_WMO_CULL=1 puts portal and distance culling back, for measuring
    /// what it costs or for chasing this again.
    bool cullingEnabled_ = false;
    float maxGroupDistance = 500.0f;
    float maxGroupDistanceSq = 250000.0f;  // maxGroupDistance^2
    float viewDistance_ = 1200.0f;
    uint32_t lastDrawCalls = 0;
    mutable uint32_t lastPortalCulledGroups = 0;
    mutable uint32_t lastDistanceCulledGroups = 0;

    // Optional query-space culling for collision/raycast hot paths.
    CollisionFocus collisionFocus;

    // Uniform grid for fast local collision queries.
    SpatialGrid spatialGrid;
    std::unordered_map<uint32_t, size_t> instanceIndexById;
    // Collision scratch buffers are thread_local (see wmo_renderer.cpp) for thread-safety.

    // Parallel visibility culling
    uint32_t numCullThreads_ = 1;

    struct InstanceDrawList {
        size_t instanceIndex;
        const ModelData* model = nullptr;     // cached pointer; saves a hashmap find per instance per frame
        std::vector<uint32_t> visibleGroups;  // group indices that passed culling
        uint32_t portalCulled = 0;
        uint32_t distanceCulled = 0;
    };
    std::vector<size_t> visibleInstances_;      // reused per frame
    std::vector<InstanceDrawList> drawLists_;    // reused per frame
    std::unordered_set<uint32_t> portalVisibleGroupSet_; // reused per frame (portal culling scratch)
    /// The camera's interior pass (updateInteriorPass): per instance id, a
    /// flag per group, and the world bounds of the groups flagged.
    std::unordered_map<uint32_t, std::vector<uint8_t>> interiorPass_;
    std::vector<std::pair<glm::vec3, glm::vec3>> interiorPassBounds_;
    uint64_t interiorPassGeneration_ = 0;

    // Collision query profiling - atomic because getFloorHeight is dispatched
    // on async threads from camera_controller while the main thread reads these.
    mutable std::atomic<double> queryTimeMs{0.0};
    mutable std::atomic<uint32_t> queryCallCount{0};

    // Floor height cache - persistent precomputed grid
    static constexpr float FLOOR_GRID_CELL_SIZE = 2.0f;  // 2 unit grid cells
    mutable std::unordered_map<uint64_t, float> precomputedFloorGrid;  // key -> floor height
    mutable uint32_t currentFrameId = 0;

    uint64_t floorGridKey(float x, float y) const {
        int32_t ix = static_cast<int32_t>(std::floor(x / FLOOR_GRID_CELL_SIZE));
        int32_t iy = static_cast<int32_t>(std::floor(y / FLOOR_GRID_CELL_SIZE));
        return (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(iy));
    }


    // Active WMO group tracking - reduces per-query group iteration
    struct ActiveGroupInfo {
        uint32_t instanceIdx = UINT32_MAX;
        uint32_t modelId = 0;
        int32_t groupIdx = -1;
        std::vector<uint32_t> neighborGroups;  // portal-connected groups
        [[nodiscard]] bool isValid() const { return instanceIdx != UINT32_MAX && groupIdx >= 0; }
        void invalidate() { instanceIdx = UINT32_MAX; groupIdx = -1; neighborGroups.clear(); }
    };
    mutable ActiveGroupInfo activeGroup_;

    // Per-frame floor height dedup cache (same XY queried 3-5x per frame)
    struct FrameFloorCache {
        static constexpr size_t CAPACITY = 16;
        struct Entry { uint64_t key; float resultZ; float normalZ; uint32_t frameId; };
        Entry entries[CAPACITY] = {};

        [[nodiscard]] uint64_t makeKey(float x, float y) const {
            // 0.5-unit quantized grid
            int32_t ix = static_cast<int32_t>(std::floor(x * 2.0f));
            int32_t iy = static_cast<int32_t>(std::floor(y * 2.0f));
            return (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) |
                   static_cast<uint64_t>(static_cast<uint32_t>(iy));
        }

        std::optional<float> get(float x, float y, uint32_t frame, float* outNormalZ = nullptr) const {
            uint64_t k = makeKey(x, y);
            size_t slot = k % CAPACITY;
            const auto& e = entries[slot];
            if (e.frameId == frame && e.key == k) {
                if (outNormalZ) *outNormalZ = e.normalZ;
                return e.resultZ;
            }
            return std::nullopt;
        }

        void put(float x, float y, float result, float normalZ, uint32_t frame) {
            uint64_t k = makeKey(x, y);
            size_t slot = k % CAPACITY;
            entries[slot] = { .key = k, .resultZ = result, .normalZ = normalZ, .frameId = frame };
        }
    };
};

} // namespace rendering
} // namespace wowee
