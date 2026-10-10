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

#include "rendering/client_liquid.hpp"
#include "rendering/client_proc_water.hpp"
#include "rendering/cube_texture.hpp"
#include "rendering/vk_texture.hpp"

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
    /// Per vertex, laid out as `heights`: the depth byte, and the stored
    /// texture coordinate a vertex-format-1 liquid carries. Empty when absent.
    std::vector<uint8_t> depths;
    std::vector<glm::vec2> storedUVs;
    /// What this client moved the heights by on its own account; the client's
    /// draw puts the surface back where the file has it.
    float clientZOffset = 0.0f;
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
 * Water renderer (Vulkan) with planar reflections, Gerstner waves,
 * GGX specular, shoreline foam, and subsurface scattering.
 */
// Matches set 2 binding 3 in water.frag.glsl. Keep kMaxWakePoints in step with
// the MAX_WAKE_POINTS constant declared there.
constexpr int kMaxWakePoints = 32;

struct WaterFrameUBOData {
    glm::mat4 reflViewProj{1.0f};
    glm::vec4 wakeBounds{0.0f};              // xy = centre, z = cull radius, w = count
    glm::vec4 wakePoints[kMaxWakePoints]{};  // xy = pos, z = age 0..1, w = strength
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
                     uint32_t momtDiffuseBGRA = 0xffffffffu);
    void removeWMOs(const std::vector<uint32_t>& wmoIds);
    void removeTile(int tileX, int tileY);
    void clear();

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

    // --- Planar reflection pass ---
    // Call sequence: beginReflectionPass → [render scene] → endReflectionPass
    bool beginReflectionPass(VkCommandBuffer cmd);
    void endReflectionPass(VkCommandBuffer cmd);

    // Get the dominant water height near a position (for reflection plane)
    std::optional<float> getDominantWaterHeight(const glm::vec3& cameraPos) const;

    // Compute reflected view matrix for a given water height
    static glm::mat4 computeReflectedView(const Camera& camera, float waterHeight);
    // Compute oblique clip projection to clip below-water geometry in reflection
    static glm::mat4 computeObliqueProjection(const glm::mat4& proj, const glm::mat4& view, float waterHeight);

    // Update the reflection UBO with reflected viewProj matrix
    void updateReflectionUBO(const glm::mat4& reflViewProj);

    /// Feed the surface disturbance left by something moving through the water.
    /// `intensity` is 0 when nothing is disturbing the surface. `wading` picks
    /// churned-up froth underfoot; swimming instead lays a V wake off the
    /// shoulders. Call once per frame.
    void updateWake(float deltaTime, const glm::vec2& pos, const glm::vec2& travelDir,
                    float intensity, bool wading);

    VkRenderPass getReflectionRenderPass() const { return reflectionRenderPass; }
    VkExtent2D getReflectionExtent() const { return {.width = REFLECTION_WIDTH, .height = REFLECTION_HEIGHT}; }
    bool hasReflectionPass() const { return reflectionRenderPass != VK_NULL_HANDLE; }
    bool hasSurfaces() const { return !surfaces.empty(); }

    void setEnabled(bool enabled) { renderingEnabled = enabled; }
    bool isEnabled() const { return renderingEnabled; }

    /// Which water is drawn. Off - the default - is the client's own liquid:
    /// its LiquidType textures, its depth ramps and its blending
    /// (client_liquid.hpp). On is this client's procedural water with
    /// reflection and refraction.
    void setEnhancedWater(bool enabled) { enhancedWater_ = enabled; }
    [[nodiscard]] bool isEnhancedWater() const { return enhancedWater_; }

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
    bool enhancedWater_ = false;
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
    void renderClient(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                      bool use1x, uint32_t frameIndex);

    void createWaterMesh(WaterSurface& surface);
    void destroyWaterMesh(WaterSurface& surface);

    glm::vec4 getLiquidColor(uint16_t liquidType) const;
    float getLiquidAlpha(uint16_t liquidType) const;

    void updateMaterialUBO(WaterSurface& surface);
    VkDescriptorSet allocateMaterialSet();
    VkExtent2D refractionCaptureExtent() const;
    void createSceneHistoryResources(VkExtent2D extent, VkFormat colorFormat, VkFormat depthFormat);
    void destroySceneHistoryResources();

    // Reflection pass resources
    void createReflectionResources();
    void destroyReflectionResources();

    VkContext* vkCtx = nullptr;

    // Pipeline
    VkPipeline waterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_WATER_SETS = 16384;

    VkSampler sceneColorSampler = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler = VK_NULL_HANDLE;
    // Per-frame scene history to avoid race between frames in flight
    static constexpr uint32_t SCENE_HISTORY_FRAMES = 2;
    // One reflection/wake UBO slot per frame in flight, at an offset every
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

    // Planar reflection resources
    static constexpr uint32_t REFLECTION_WIDTH = 512;
    static constexpr uint32_t REFLECTION_HEIGHT = 512;
    VkRenderPass reflectionRenderPass = VK_NULL_HANDLE;
    VkFramebuffer reflectionFramebuffer = VK_NULL_HANDLE;
    VkImage reflectionColorImage = VK_NULL_HANDLE;
    VmaAllocation reflectionColorAlloc = VK_NULL_HANDLE;
    VkImageView reflectionColorView = VK_NULL_HANDLE;
    VkImage reflectionDepthImage = VK_NULL_HANDLE;
    VmaAllocation reflectionDepthAlloc = VK_NULL_HANDLE;
    VkImageView reflectionDepthView = VK_NULL_HANDLE;
    VkSampler reflectionSampler = VK_NULL_HANDLE;
    VkImageLayout reflectionColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Reflection UBO (mat4 reflViewProj)
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

    ::VkBuffer reflectionUBO = VK_NULL_HANDLE;
    VmaAllocation reflectionUBOAlloc = VK_NULL_HANDLE;
    void* reflectionUBOMapped = nullptr;

    // Separate 1x water pass (used when MSAA is active)
    VkRenderPass water1xRenderPass = VK_NULL_HANDLE;
    VkPipeline water1xPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> water1xFramebuffers;

    std::vector<WaterSurface> surfaces;
    bool renderingEnabled = true;
    bool refractionEnabled = false;
    VkExtent2D renderExtent_{.width = 0, .height = 0};
};

} // namespace rendering
} // namespace wowee
