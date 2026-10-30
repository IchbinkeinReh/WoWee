#pragma once

#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <string>
#include <unordered_map>

#include "audio/zone_ambience.hpp"
#include "rendering/client_liquid.hpp"
#include "rendering/client_proc_water.hpp"
#include "rendering/minimap_indoor.hpp"
#include "rendering/cube_texture.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/rt_glow.hpp"

namespace wowee {
namespace pipeline {
    struct ADTTerrain;
    struct LiquidData;
    struct WMOLiquid;
    class AssetManager;
}

namespace rendering {

class Camera;
class VkContext;
class VkTexture;

/**
 * Water surface for a single map chunk
 */
struct WaterSurface {
    glm::vec3 position;
    glm::vec3 origin;
    glm::vec3 stepX;
    glm::vec3 stepY;
    float minHeight;
    float maxHeight;
    uint16_t liquidType;

    int tileX = -1, tileY = -1;
    uint32_t wmoId = 0;
    /// The building's group the liquid is in (MOGI's index).
    uint32_t wmoGroup = 0;
    /// A building's liquid in the building's space, as its search reads it
    /// (0x0079b760): the world to the building, the corner, each vertex's
    /// height and each tile's flags.
    glm::mat4 wmoWorldToLocal{1.0f};
    glm::vec2 wmoLocalCorner{0.0f};
    std::vector<float> wmoLocalHeights;
    std::vector<uint8_t> wmoTileFlags;

    uint8_t xOffset = 0;
    uint8_t yOffset = 0;
    uint8_t width = 8;
    uint8_t height = 8;

    std::vector<float> heights;
    std::vector<uint8_t> mask;

    // Vulkan render data
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    int indexCount = 0;

    // Per-surface material UBO
    ::VkBuffer materialUBO = VK_NULL_HANDLE;
    VmaAllocation materialAlloc = VK_NULL_HANDLE;

    // Material descriptor set (set 1)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    // --- The client's own liquid (client_liquid.hpp) ---
    /// The LiquidType.dbc row this is drawn with. MH2O names it outright; MCLQ
    /// and the merged tiles carry the basic type, turned into its row (1-4).
    uint32_t clientLiquidType = 0;
    /// A magma surface's glow, as lights for the ray traced lighting
    /// (rt_lights::liquidGlowPatches), in the world; empty for any other liquid.
    std::vector<rt_lights::GlowPatch> lavaPatches;
    /// Per vertex, laid out as `heights`: the depth byte, and the stored
    /// texture coordinate a vertex-format-1 liquid carries. Empty when absent.
    std::vector<uint8_t> depths;
    std::vector<glm::vec2> storedUVs;
    /// How far the enhanced water draws this surface off the file's height:
    /// -1 for the old Stormwind lowering, which only that draw keeps. The
    /// heights, and so every height query - swimming, the camera under the
    /// surface, the underwater fog - stay where the file and the client's
    /// own draw have them.
    float enhancedZOffset = 0.0f;
    /// A WMO liquid's vertices where the file has them, each with its own
    /// height (0x007a7cc0); this client draws its own surface flat.
    std::vector<glm::vec3> clientPositions;
    /// A WMO liquid drawn the interior way (0x00793d20), and the MOMT colour
    /// its vertices take then.
    bool wmoInterior = false;
    uint32_t wmoColorBGRA = 0xffffffffu;
    /// The client's vertices (position, surface coordinate, depth coordinate,
    /// colour), drawn with the index buffer above.
    ::VkBuffer clientVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation clientVertexAlloc = VK_NULL_HANDLE;

    [[nodiscard]] bool hasHeightData() const { return !heights.empty(); }
};

/**
 * Water renderer (Vulkan): the client's own liquids, and with ray tracing
 * on the traced water with Gerstner waves,
 * GGX specular, shoreline foam, and subsurface scattering.
 */
// Matches set 2 binding 3 in water.frag.glsl. Keep kMaxWakePoints in step with
// the MAX_WAKE_POINTS constant declared there.
constexpr int kMaxWakePoints = 32;

/// The traced water's waves at their default (rtwaveamp 100%): half the
/// height they were first made with, which in the traced mirror read as a
/// choppy sea on a still pond.
inline constexpr float kRtWaveAmpScale = 0.5f;

struct WaterFrameUBOData {
    glm::vec4 wakeBounds{0.0f};              // xy = centre, z = cull radius, w = count
    glm::vec4 wakePoints[kMaxWakePoints]{};  // xy = pos, z = age 0..1, w = strength
    // The ray traced water (water_rt.glsli): x on, y the view (1 the
    // reflection alone, 2 what comes through alone), z how far a reflected
    // ray goes, w 1 while the traced scene holds anything.
    glm::vec4 rtWater{0.0f};
    glm::vec4 rtWater2{0.0f};      // x how far a refracted ray goes, z/w the waves' height and frequency
    glm::vec4 rtSkyTop{0.0f};      // the sky a reflection leaving the scene meets, overhead
    glm::vec4 rtSkyHorizon{0.0f};  // and at the horizon
};

/// What the ray traced water needs from the frame (Renderer, each frame).
struct RtWaterFrame {
    bool on = false;
    VkDescriptorSet sceneSet = VK_NULL_HANDLE;  ///< RtScene's, this frame
    bool sceneHasContent = false;
    int view = 0;               ///< 1 the reflection alone, 2 what comes through
    float waveAmp = 1.0f;       ///< the waves' height, 1 the default
    float waveFreq = 1.0f;      ///< and their frequency
    float reflectReach = 400.0f;
    float refractReach = 80.0f;
    glm::vec3 skyTop{0.5f, 0.7f, 1.0f};
    glm::vec3 skyHorizon{0.7f, 0.75f, 0.8f};
};

class WaterRenderer {
public:
    WaterRenderer();
    ~WaterRenderer();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();

    void loadFromTerrain(const pipeline::ADTTerrain& terrain, bool append = false,
                         int tileX = -1, int tileY = -1);

    /// `momtDiffuseBGRA`: the diffuse colour of the MOMT entry the liquid
    /// names, which an interior liquid's vertices take (0x00793d20).
    void loadFromWMO(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix, uint32_t wmoId,
                     uint32_t momtDiffuseBGRA = 0xffffffffu, uint32_t wmoGroup = 0);
    void removeWMOs(const std::vector<uint32_t>& wmoIds);
    void removeTile(int tileX, int tileY);
    void clear();

    /// The lights the lava near `eye` gives the ray traced lighting: each
    /// magma surface's glowing patches (rt_lights::liquidGlowPatches) whose
    /// light reaches within `range`, nearest first, at most kMaxLavaLights.
    void collectRtLights(const glm::vec3& eye, float range, std::vector<rt_lights::PointLight>& out) const;
    /// What the liquid at (x, y) is made of (client_liquid::liquidKindOfRow):
    /// lava and slime are Magma, and take no water's swim effects. None with
    /// no liquid there.
    client_liquid::MaterialKind getLiquidKindAt(float glX, float glY) const;
    static constexpr size_t kMaxLavaLights = 32;

    void recreatePipelines();

    // Separate 1x pass for MSAA mode - water rendered after MSAA resolve
    bool createWater1xPass(VkFormat colorFormat, VkFormat depthFormat);
    void createWater1xFramebuffers(const std::vector<VkImageView>& swapViews,
                                    VkImageView depthView, VkExtent2D extent);
    void destroyWater1xResources();
    bool hasWater1xPass() const { return water1xRenderPass != VK_NULL_HANDLE; }
    VkRenderPass getWater1xRenderPass() const { return water1xRenderPass; }
    VkFramebuffer getWater1xFramebuffer(uint32_t index) const {
        return index < water1xFramebuffers.size() ? water1xFramebuffers[index] : VK_NULL_HANDLE;
    }

    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera, float time, bool use1x = false, uint32_t frameIndex = 0);
    void captureSceneHistory(VkCommandBuffer cmd,
                             VkImage srcColorImage,
                             VkImage srcDepthImage,
                             VkExtent2D srcExtent,
                             bool srcDepthIsMsaa,
                             uint32_t frameIndex = 0,
                             VkExtent2D srcDepthExtent = {});  ///< zero: srcExtent

    /// Feed the surface disturbance left by something moving through the water.
    /// `intensity` is 0 when nothing is disturbing the surface. `wading` picks
    /// churned-up froth underfoot; swimming instead lays a V wake off the
    /// shoulders. Call once per frame.
    void updateWake(float deltaTime, const glm::vec2& pos, const glm::vec2& travelDir,
                    float intensity, bool wading);

    bool hasSurfaces() const { return !surfaces.empty(); }

    void setEnabled(bool enabled) { renderingEnabled = enabled; }
    bool isEnabled() const { return renderingEnabled; }

    /// The ray traced water's scene set layout (RtScene) and which tracer it
    /// was made for; once, after both exist. Its pipelines are built when
    /// first drawn with.
    void setRtWaterScene(VkDescriptorSetLayout sceneLayout, bool hardware);
    /// This frame's ray traced water, before render().
    void setRtWaterFrame(const RtWaterFrame& frame) { rtFrame_ = frame; }
    /// Whether this frame's water and ocean are the traced ones: on, with
    /// what they need. Otherwise every liquid is the client's own (its
    /// LiquidType textures, depth ramps and blending, client_liquid.hpp), as
    /// magma and slime always are.
    [[nodiscard]] bool rtWaterDrawn() const {
        return rtFrame_.on && rtFrame_.sceneSet != VK_NULL_HANDLE && rtPipelineLayout_ != VK_NULL_HANDLE;
    }

    /// LiquidType.dbc and LiquidMaterial.dbc, for the client's liquid. Textures
    /// are loaded on a type's first draw.
    void loadClientLiquids(pipeline::AssetManager* assetManager);

    void setRefractionEnabled(bool enabled);
    bool isRefractionEnabled() const { return refractionEnabled; }
    // Display brightness (1.0 = neutral). The scene-history capture used for
    // refraction bakes this in, so the shader divides it back out.
    // Size of the target the water is drawn into. Screen-space lookups derive
    // their UVs from this rather than from the refraction texture's own size,
    // which is deliberately smaller than the frame.
    void setRenderExtent(VkExtent2D e) { renderExtent_ = e; }

    /// Where a point sits on a surface, if that surface has water there:
    /// the projection and the render mask, which every query asks first.
    std::optional<glm::vec2> wateredGridPosition(const WaterSurface& surface,
                                                 float glX, float glY) const;

    std::optional<float> getWaterHeightAt(float glX, float glY) const;
    /// Like getWaterHeightAt but only returns water surfaces whose height is
    /// close to the query Z (within maxAbove units above). Avoids false
    /// underwater detection from elevated WMO water far above the camera.
    std::optional<float> getNearestWaterHeightAt(float glX, float glY, float queryZ, float maxAbove = 15.0f) const;
    std::optional<uint16_t> getWaterTypeAt(float glX, float glY) const;
    /// Every liquid cell whose middle - its centre, half way up between the
    /// liquid's lowest and highest - is within `radius` of `at`, with its
    /// LiquidType row (0x0077f390 -> 0x0079c360): the ground's liquid where
    /// `ground`. Ahead of it the buildings' groups in `groups` - the ones the
    /// portal walk from the player's met (0x007a1b10), in its order - give
    /// their nearest tile of each LiquidType, at any distance (0x0079b760).
    std::vector<audio::zone_ambience::LiquidCell>
    liquidCellsNear(const glm::vec3& at, float radius, bool ground,
                    const std::vector<minimap_indoor::GroupRef>& groups) const;
    bool isWmoWaterAt(float glX, float glY) const;

    int getSurfaceCount() const { return static_cast<int>(surfaces.size()); }

    /// The light's water colours and alphas this frame (0x008a2bf0 builds
    /// the liquid ramps from them): an ocean runs from ch14 at alpha
    /// OceanShallowAlpha in the shallows to ch15 at OceanDeepAlpha in the
    /// deep, rivers and lakes from ch16 / WaterShallowAlpha to ch17 /
    /// WaterDeepAlpha. Magma and slime keep their own.
    struct LightWaterColors {
        glm::vec4 oceanClose{0.0f};   // rgb, a = alpha
        glm::vec4 oceanFar{0.0f};
        glm::vec4 riverClose{0.0f};
        glm::vec4 riverFar{0.0f};
        /// ch9, which the world light carries as its specular colour
        /// (0x007816f0 copies the light's +0xf8 into it) and
        /// psLiquidWater's highlight is tinted by.
        glm::vec3 sunColor{1.0f};
    };
    /// The client's 'specular' option (off by default).
    void setClientSpecular(bool on) { clientSpecular_ = on; }
    void setLightWaterColors(const LightWaterColors& colors) {
        lightWaterColors_ = colors;
        hasLightWaterColors_ = true;
    }

private:
    LightWaterColors lightWaterColors_;
    bool hasLightWaterColors_ = false;
    /// The client's 'specular' option (0x0078de60 sets world flag 0x8000000),
    /// off by default. 0x00781430 hands it, with the pixel shader flag
    /// 0x10000000, to 0xb23f68 through 0xce04a0 (0x007bd8a0), and 0x008a1fa0
    /// picks CMaterialWater over CMaterialWaterNoSpec by it.
    bool clientSpecular_ = false;
    double clientTimeSeconds_ = 0.0;

    // --- The client's own liquid ---
    struct ClientTexSlot {
        bool loaded = false;
        client_liquid::ProceduralTex procedural = client_liquid::ProceduralTex::None;
        std::vector<std::unique_ptr<VkTexture>> frames;
        std::vector<VkDescriptorSet> sets;  // one per frame, set 1 or 2 layout
        /// Procedural water's cube units (0 and 1): one cube a frame.
        std::vector<CubeTexture> cubes;
    };
    struct ClientLiquid {
        client_liquid::LiquidTypeRecord record;
        int32_t lvf = 0;
        client_liquid::MaterialKind kind = client_liquid::MaterialKind::Water;
        std::array<ClientTexSlot, 6> slots;
    };
    std::unordered_map<uint32_t, client_liquid::LiquidTypeRecord> clientTypeRecords_;
    std::unordered_map<uint32_t, client_liquid::LiquidMaterialRecord> clientMaterials_;
    std::unordered_map<uint32_t, ClientLiquid> clientLiquids_;
    pipeline::AssetManager* assetManager_ = nullptr;

    VkDescriptorSetLayout clientTexSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout clientRampSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool clientTexPool_ = VK_NULL_HANDLE;
    VkDescriptorPool clientRampPool_ = VK_NULL_HANDLE;
    VkPipelineLayout clientPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline clientWaterPipeline_ = VK_NULL_HANDLE;
    VkPipeline clientMagmaPipeline_ = VK_NULL_HANDLE;
    VkPipeline clientWater1xPipeline_ = VK_NULL_HANDLE;
    VkPipeline clientMagma1xPipeline_ = VK_NULL_HANDLE;
    std::unique_ptr<VkTexture> clientWhiteTex_;
    VkDescriptorSet clientWhiteSet_ = VK_NULL_HANDLE;
    static constexpr VkDeviceSize kRampUBOSize = 192 * sizeof(glm::vec4);
    ::VkBuffer clientRampUBO_ = VK_NULL_HANDLE;
    VmaAllocation clientRampAlloc_ = VK_NULL_HANDLE;
    void* clientRampMapped_ = nullptr;
    VkDescriptorSet clientRampSets_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // Procedural water (CMaterialProcWater, 0x008a48f0): six units and its
    // constants in one set, drawn sets allocated afresh each frame.
    VkDescriptorSetLayout procSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout procPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline clientProcPipeline_ = VK_NULL_HANDLE;
    VkPipeline clientProc1xPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool procPools_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    static constexpr uint32_t kProcSetsPerFrame = 32;
    static constexpr VkDeviceSize kProcUBOStride = 512;
    ::VkBuffer procUBO_ = VK_NULL_HANDLE;
    VmaAllocation procAlloc_ = VK_NULL_HANDLE;
    void* procMapped_ = nullptr;
    VkSampler procCubeSampler_ = VK_NULL_HANDLE;
    std::unique_ptr<VkTexture> procGreenTex_;
    ClientTexSlot procGreenCube_;
    client_proc_water::WaveManager waves_;
    client_proc_water::CrtRand waveRand_;

    bool initProcWater(VkDescriptorSetLayout perFrameLayout);
    void destroyProcWater();
    void loadProcCubeSlot(ClientTexSlot& slot, const std::string& name);

    bool initClientLiquid(VkDescriptorSetLayout perFrameLayout);
    void destroyClientLiquid();
    /// Both client pipelines against one pass; shared by initialize,
    /// recreatePipelines and the 1x pass, so the three cannot disagree.
    void buildClientPipelines(VkRenderPass pass, VkSampleCountFlagBits samples,
                              VkPipeline& water, VkPipeline& magma, VkPipeline& proc);
    ClientLiquid* clientLiquidFor(uint32_t liquidType);
    void loadClientSlot(ClientTexSlot& slot, const std::string& name);
    void createClientMesh(WaterSurface& surface);
    void writeClientRamps(uint32_t frameSlot);
    /// `tracedWater`: the traced water draws the water and the ocean, so
    /// only the magma and the slime are left here.
    void renderClient(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                      bool use1x, uint32_t frameIndex, bool tracedWater);

    void createWaterMesh(WaterSurface& surface);
    void destroyWaterMesh(WaterSurface& surface);

    glm::vec4 getLiquidColor(uint16_t liquidType) const;
    float getLiquidAlpha(uint16_t liquidType) const;

    void updateMaterialUBO(WaterSurface& surface);
    VkDescriptorSet allocateMaterialSet();
    VkExtent2D refractionCaptureExtent() const;
    void createSceneHistoryResources(VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat);
    void destroySceneHistoryResources();

    // The per-frame block (WaterFrameUBOData), a slot a frame in flight.
    void createFrameUBO();
    void destroyFrameUBO();

    VkContext* vkCtx = nullptr;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    // The ray traced water's: the scene's set as set 3, and its pipelines
    // (water.frag, or water_rt_hw.frag for ray queries) against the scene
    // pass and the 1x pass.
    VkDescriptorSetLayout rtSceneLayout_ = VK_NULL_HANDLE;
    bool rtHardware_ = false;
    VkPipelineLayout rtPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline rtPipeline_ = VK_NULL_HANDLE;
    VkPipeline rt1xPipeline_ = VK_NULL_HANDLE;
    RtWaterFrame rtFrame_;
    VkPipeline buildWaterPipeline(VkRenderPass pass, VkSampleCountFlagBits samples, const char* fragPath,
                                  VkPipelineLayout layout);
    void destroyRtPipelines();
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;  ///< the renderer's set 0, kept for the traced water's layout
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_WATER_SETS = 16384;

    VkSampler sceneColorSampler = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler = VK_NULL_HANDLE;
    // Per-frame scene history to avoid race between frames in flight
    static constexpr uint32_t SCENE_HISTORY_FRAMES = 2;
    // One frame UBO slot per frame in flight, at an offset every
    // device accepts for a uniform buffer (Vulkan caps the alignment at 256).
    static constexpr VkDeviceSize kFrameUBOStride =
        (sizeof(WaterFrameUBOData) + 255) / 256 * 256;
    struct PerFrameSceneHistory {
        VkImage colorImage = VK_NULL_HANDLE;
        VmaAllocation colorAlloc = VK_NULL_HANDLE;
        VkImageView colorView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VmaAllocation depthAlloc = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        VkDescriptorSet sceneSet = VK_NULL_HANDLE;
    };
    PerFrameSceneHistory sceneHistory[SCENE_HISTORY_FRAMES];
    VkExtent2D sceneHistoryExtent = {.width = 0, .height = 0};
    bool sceneHistoryReady = false;
    mutable uint32_t renderDiagCounter_ = 0;

    // Surface disturbance trail. Points age out, spreading as they go; the
    // per-point drift is what turns a swimmer's pair of emissions into a V.
    struct WakePoint {
        glm::vec2 pos{0.0f};
        glm::vec2 drift{0.0f};
        float age = 0.0f;        // seconds
        float life = 1.0f;       // seconds
        float strength = 0.0f;
    };
    std::vector<WakePoint> wakePoints_;
    WaterFrameUBOData frameUBO_{};
    glm::vec2 lastWakeEmitPos_{0.0f};
    bool hasWakeEmitPos_ = false;

    void uploadFrameUBO();

    ::VkBuffer frameUBOBuffer_ = VK_NULL_HANDLE;
    VmaAllocation frameUBOAlloc_ = VK_NULL_HANDLE;
    void* frameUBOMapped_ = nullptr;

    // Separate 1x water pass (used when MSAA is active)
    VkRenderPass water1xRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> water1xFramebuffers;

    std::vector<WaterSurface> surfaces;
    bool renderingEnabled = true;
    bool refractionEnabled = false;
    VkExtent2D renderExtent_{.width = 0, .height = 0};
};

} // namespace rendering
} // namespace wowee
