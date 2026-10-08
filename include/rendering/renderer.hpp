#pragma once

#include "rendering/blob_shadow.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <future>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "rendering/pass_ablation.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/sky_system.hpp"
#include "core/screen_recorder.hpp"
#include "pipeline/custom_zone_discovery.hpp"

#include "pipeline/grass_biomes.hpp"
#include "pipeline/grass_clearing.hpp"
#include "pipeline/grass_population.hpp"
#include "pipeline/grass_profile.hpp"

namespace wowee {
namespace core { class Window; }
namespace rendering { class VkContext; }
namespace game { class World; class ZoneManager; class GameHandler; }
namespace audio { class AudioCoordinator; }
namespace pipeline { class AssetManager; }

namespace rendering {

class Camera;
class CameraController;
class TerrainRenderer;
class TerrainManager;
class PerformanceHUD;
class WaterRenderer;
class Skybox;
class Celestial;
class StarField;
class Clouds;
class LensFlare;
class Weather;
class Lightning;
class LightingManager;
class SwimEffects;
class MountDust;
class FishingLineRenderer;
class LevelUpEffect;
class ChargeEffect;
class CharacterRenderer;
class WMORenderer;
class M2Renderer;
class Minimap;
namespace world_map { class WorldMapFacade; }
using WorldMap = world_map::WorldMapFacade;
class QuestMarkerRenderer;
class FootprintRenderer;
class BlobShadowRenderer;
class CharacterPreview;
class AmdFsr3Runtime;
class SpellVisualSystem;
class PostProcessPipeline;
class AnimationController;
class LevelUpEffect;
class ChargeEffect;
class SwimEffects;
class RenderGraph;
class OverlaySystem;
class HiZSystem;
class GrassRenderer;
class VolumetricFog;
class RtScene;
class RtLighting;
class LootSparkles;
class SunShafts;
class ScreenEffects;
class ScreenCapture;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool initialize(core::Window* window);
    void shutdown();

    void beginFrame();
    void endFrame();

    /// Recorded after the interface is drawn and before the frame is submitted,
    /// into the frame's own command buffer. A second window draws here - see
    /// AuxSwapchain - so the frame's one fence covers it. Empty clears it.
    void setAfterInterfaceRecorder(std::function<void(VkCommandBuffer)> recorder) {
        afterInterface_ = std::move(recorder);
    }

    void renderWorld(game::World* world, game::GameHandler* gameHandler = nullptr);

    /**
     * Update renderer (camera, etc.)
     */
    void update(float deltaTime);

    /**
     * Load test terrain for debugging
     * @param assetManager Asset manager to load terrain data
     * @param adtPath Path to ADT file (e.g., "World\\Maps\\Azeroth\\Azeroth_32_49.adt")
     */
    bool loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath);

    /**
     * Initialize all sub-renderers (WMO, M2, Character, terrain, water, minimap, etc.)
     * without loading any ADT tile.  Used by WMO-only maps (dungeons/raids/BGs).
     */
    bool initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName);
    /// The map every sub-renderer is looking at.
    ///
    /// initializeRenderers set this on all four when it built them, and it
    /// builds them once - so on every later map change the two the loader
    /// remembered by hand moved and the two it did not stayed on the map the
    /// session started in. The minimap was one of those: in a dungeon it went
    /// on asking for the tiles of wherever the player had logged in, and got
    /// none, which is a minimap with no dungeon on it.
    void setActiveMapName(const std::string& name);

    /**
     * Enable/disable terrain rendering
     */
    void setTerrainEnabled(bool enabled) { terrainEnabled = enabled; }

    /**
     * Enable/disable wireframe mode
     */
    void setWireframeMode(bool enabled);



    /**
     * Render performance HUD
     */
    void renderHUD();

    Camera* getCamera() { return camera.get(); }
    CameraController* getCameraController() { return cameraController.get(); }
    TerrainRenderer* getTerrainRenderer() const { return terrainRenderer.get(); }
    TerrainManager* getTerrainManager() const { return terrainManager.get(); }
    PerformanceHUD* getPerformanceHUD() { return performanceHUD.get(); }
    WaterRenderer* getWaterRenderer() const { return waterRenderer.get(); }
    Skybox* getSkybox() const { return skySystem ? skySystem->getSkybox() : nullptr; }
    Celestial* getCelestial() const { return skySystem ? skySystem->getCelestial() : nullptr; }
    StarField* getStarField() const { return skySystem ? skySystem->getStarField() : nullptr; }
    Clouds* getClouds() const { return skySystem ? skySystem->getClouds() : nullptr; }
    LensFlare* getLensFlare() const { return skySystem ? skySystem->getLensFlare() : nullptr; }
    Weather* getWeather() const { return weather.get(); }
    Lightning* getLightning() const { return lightning.get(); }
    CharacterRenderer* getCharacterRenderer() const { return characterRenderer.get(); }
    WMORenderer* getWMORenderer() const { return wmoRenderer.get(); }
    M2Renderer* getM2Renderer() const { return m2Renderer.get(); }
    Minimap* getMinimap() const { return minimap.get(); }
    WorldMap* getWorldMap() const { return worldMap.get(); }
    QuestMarkerRenderer* getQuestMarkerRenderer() const { return questMarkerRenderer.get(); }
    FootprintRenderer* getFootprintRenderer() const { return footprintRenderer.get(); }
    FishingLineRenderer* getFishingLineRenderer() const { return fishingLines_.get(); }
    SkySystem* getSkySystem() const { return skySystem.get(); }
    const std::string& getCurrentZoneName() const;
    uint32_t getCurrentZoneId() const;
    /// The area under the player, asked of AreaTable rather than resolved to a
    /// zone first - the world PvP flag is on the subzone. See ZoneManager.
    bool isOnOutdoorPvpObjective() const;
    bool isPlayerIndoors() const { return playerIndoors_; }
    VkContext* getVkContext() const { return vkCtx; }
    VkDescriptorSetLayout getPerFrameSetLayout() const { return perFrameSetLayout; }
    /// What a per-frame set allocated elsewhere binds at binding 2: a fog
    /// volume of clear air, for a set whose block leaves the fog off.
    VkImageView getNeutralFogVolumeView() const;
    /// What a per-frame set allocated elsewhere binds at 3 and 4.
    VkImageView getNeutralRtLightingView() const;
    /// The geometry the ray traced lighting sees; renderers register with it.
    RtScene* getRtScene() const { return rtScene_.get(); }
    VkRenderPass getShadowRenderPass() const { return shadowRenderPass; }

    // Third-person character follow
    void setCharacterFollow(uint32_t instanceId);
    glm::vec3& getCharacterPosition() { return characterPosition; }
    uint32_t getCharacterInstanceId() const { return characterInstanceId; }
    float getCharacterYaw() const { return characterYaw; }
    void setCharacterYaw(float yawDeg) { characterYaw = yawDeg; }

    // Screenshot capture - copies swapchain image to PNG file
    bool captureScreenshot(const std::string& outputPath);

    /// Start writing the screen, interface included, and what the client
    /// plays to a video file. False, with the reason in error, when it cannot.
    bool startRecording(const std::string& path, std::string& error);
    /// Finish the file and close it.
    core::ScreenRecorder::Stats stopRecording();
    [[nodiscard]] bool isRecording() const;
    /// Why a recording stopped by itself - the disk filled, the encoder gave
    /// up - once, for the interface to say; empty otherwise. The file is
    /// finished properly all the same.
    std::string takeRecordingFailure();

    // Spell visual effects (SMSG_PLAY_SPELL_VISUAL / SMSG_PLAY_SPELL_IMPACT)
    // Delegates to SpellVisualSystem (owned by Renderer)
    SpellVisualSystem* getSpellVisualSystem() const { return spellVisualSystem_.get(); }

    // Combat visual state (compound: resets AnimationController + SpellVisualSystem)
    void resetCombatVisualState();

    // Sub-system accessors (§4.2)
    AnimationController* getAnimationController() const { return animationController_.get(); }
    LevelUpEffect* getLevelUpEffect() const { return levelUpEffect.get(); }
    LootSparkles* getLootSparkles() const { return lootSparkles_.get(); }
    ChargeEffect* getChargeEffect() const { return chargeEffect.get(); }
    SwimEffects* getSwimEffects() const { return swimEffects.get(); }

    // Selection circle for targeted entity
    void setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color);
    void clearSelectionCircle();

    // CPU timing stats (milliseconds, last frame).
    double getLastUpdateMs() const { return lastUpdateMs; }
    double getLastRenderMs() const { return lastRenderMs; }
    double getLastCameraUpdateMs() const { return lastCameraUpdateMs; }
    double getLastTerrainRenderMs() const { return lastTerrainRenderMs; }
    double getLastWMORenderMs() const { return lastWMORenderMs; }
    double getLastM2RenderMs() const { return lastM2RenderMs; }
    // Audio coordinator - owned by Application, set via setAudioCoordinator().
    void setAudioCoordinator(audio::AudioCoordinator* ac) { audioCoordinator_ = ac; }
    audio::AudioCoordinator* getAudioCoordinator() { return audioCoordinator_; }
    game::ZoneManager* getZoneManager() { return zoneManager.get(); }
    LightingManager* getLightingManager() { return lightingManager.get(); }

    const std::vector<pipeline::CustomZoneInfo>& getCustomZones() const { return customZones_; }

private:
    std::function<void(VkCommandBuffer)> afterInterface_;

    // True when water is drawn in the scene continuation pass rather than in
    // the scene pass itself (see renderWorld).
    bool waterDrawsInContinuePass() const;

    /// Brightness and the minimap, in that order, at the end of
    /// the scene pass. The threaded and single-threaded paths both finish this
    /// way and differ only in which command buffer they are recording into.
    /// The underwater tint and its waterline. One implementation, called
    /// from both the parallel and the fallback recording paths, which had
    /// carried different ones.
    void renderUnderwaterOverlay(VkCommandBuffer cmd);
    void renderPostSceneOverlays(VkCommandBuffer cmd, game::GameHandler* gameHandler);
    void renderMinimapOverlay(VkCommandBuffer cmd, game::GameHandler* gameHandler);

    /// Point the swim spray at whichever pass the water ends up drawing in, so
    /// it can be recorded after the water rather than under it. Must run before
    /// the spray's pipelines are built, and again whenever they are rebuilt.
    void syncSwimEffectsTargetPass();
    /// syncSwimEffectsTargetPass, then rebuild the spray or the minimap if the
    /// answer moved either of them. Runs from beginFrame every frame, once the
    /// post-process target for the frame is settled; free when nothing moved.
    void refreshSwimEffectsPass();

    /// True when the spray's pipelines were built for the water continuation
    /// pass. Draw sites test this rather than waterDrawsInContinuePass() so a
    /// mid-run mode change cannot record a pipeline into an incompatible pass.
    bool swimEffectsDrawWithWater_ = false;
    /// Whether the minimap follows water out of the scene pass, for the same
    /// reason the spray does - see syncSwimEffectsTargetPass.
    bool minimapDrawsWithWater_ = false;
    /// Whether the sun's and the White Lady's glare follows water out of the
    /// scene pass: the client draws it after all of the world, water too
    /// (0x004f8ea0 calls 0x007f0870 after 0x0077f980 and the rest).
    bool glareDrawsWithWater_ = false;

    void runDeferredWorldInitStep(float deltaTime);

    core::Window* window = nullptr;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<CameraController> cameraController;
    std::unique_ptr<TerrainRenderer> terrainRenderer;
    /// How much of the line from the eye to the sun is blocked, smoothed.
    ///
    /// Sampled once a frame in update() and eased, because the raw answer is a
    /// yes or a no and a hill edge crossing it would snap the flare on and off.
    float sunOcclusion_ = 0.0f;
    /// The same toward the White Lady, for her glare.
    float moonOcclusion_ = 0.0f;

    /// The line toward `dir`, asked of the terrain, the buildings and where
    /// the camera is. The client asks a hardware occlusion query (0x009abe00).
    /// `why`, when given, is told which test decided it (WOWEE_GLARE_DIAG).
    [[nodiscard]] float sampleSunOcclusion(const glm::vec3& dir, std::string* why = nullptr) const;
    /// WOWEE_GLARE_DIAG: seconds until the next log of the sun's occlusion.
    float glareDiagTimer_ = 0.0f;

    std::unique_ptr<TerrainManager> terrainManager;
    std::unique_ptr<PerformanceHUD> performanceHUD;
    std::unique_ptr<WaterRenderer> waterRenderer;
    std::unique_ptr<Skybox> skybox;
    std::unique_ptr<Celestial> celestial;
    std::unique_ptr<StarField> starField;
    std::unique_ptr<Clouds> clouds;
    std::unique_ptr<LensFlare> lensFlare;
    std::unique_ptr<Weather> weather;
    std::unique_ptr<Lightning> lightning;
    std::unique_ptr<LightingManager> lightingManager;
    std::unique_ptr<SkySystem> skySystem;  // Coordinator for sky rendering
    std::unique_ptr<SwimEffects> swimEffects;
    std::unique_ptr<MountDust> mountDust;
    std::unique_ptr<LevelUpEffect> levelUpEffect;
    std::unique_ptr<LootSparkles> lootSparkles_;
    std::unique_ptr<ChargeEffect> chargeEffect;
    std::unique_ptr<FishingLineRenderer> fishingLines_;
    std::unique_ptr<CharacterRenderer> characterRenderer;
    std::unique_ptr<WMORenderer> wmoRenderer;
    std::unique_ptr<M2Renderer> m2Renderer;
    std::unique_ptr<M2Renderer> skyboxModelRenderer_;
    /// The last zone a terrain chunk actually named. Many chunks carry an area
    /// id of zero, and a tile being loaded carries none at all, so the lookup
    /// answers "do not know" often - and answering from somewhere else instead
    /// made the zone flip at chunk boundaries while walking. Mutable because
    /// getCurrentZoneId() is const and this is a cache of what it last learnt.
    mutable uint32_t lastResolvedZoneId_ = 0;
    /// The map that answer belongs to, so it is dropped on a
    /// continent change rather than held across one.
    mutable uint32_t lastResolvedZoneMapId_ = 0xFFFFFFFFu;

    /// The original client's sky models that are up, one instance each, faded
    /// by the weight LightingManager gives them. See updateSkyboxLayers.
    struct SkyLayerInstance {
        std::string path;
        uint32_t instanceId = 0;
    };
    std::vector<SkyLayerInstance> skyLayers_;
    /// Sky models already uploaded, by path, so one that fades out and back in
    /// is not read off disk again.
    std::unordered_map<std::string, uint32_t> loadedSkyModels_;
    /// Sky paths that did not resolve to a usable model, so a failing path is
    /// not read off disk again on every frame it is wanted.
    std::unordered_set<std::string> failedSkyboxPaths_;
    /// The client's stars (Environments\\Stars\\stars.mdl), a sky model of
    /// their own. 0x007f09b0 draws them first, under the dome, the sun, the
    /// moons and the clouds, where the LightSkybox models go on top of all
    /// of it - so they have a renderer of their own rather than a slot in
    /// skyboxModelRenderer_ (0x009abb00, 0x009abd50).
    std::unique_ptr<M2Renderer> starsModelRenderer_;
    uint32_t starsInstanceId_ = 0;
    bool starsModelTried_ = false;
    /// How much of the stars shows this frame, 0 when the client would not
    /// draw them (daynight::starsAlpha).
    float starsAlpha_ = 0.0f;
    std::unique_ptr<Minimap> minimap;
    std::unique_ptr<WorldMap> worldMap;
    std::unique_ptr<QuestMarkerRenderer> questMarkerRenderer;
    std::unique_ptr<FootprintRenderer> footprintRenderer;
    std::unique_ptr<BlobShadowRenderer> blobShadowRenderer;
    std::vector<blob_shadow::Caster> blobCasters_;
    audio::AudioCoordinator* audioCoordinator_ = nullptr;  // Owned by Application
    std::unique_ptr<AnimationController> animationController_;  // §4.2
    std::unique_ptr<game::ZoneManager> zoneManager;
    // Shadow mapping (Vulkan)
    /// The side of one cascade's tile of the shadow atlas, as it is built now.
    /// With one cascade the atlas is this square and nothing else, which is
    /// the single map this renderer always had.
    ///
    /// It starts at the quality level's size, chosen before the per-frame
    /// resources are built. The shadow resolution setting can raise it from
    /// there mid-session: setShadowResolutionScale asks, and the next frame
    /// starts by rebuilding the atlas (applyPendingShadowMapSize). The cascade
    /// count is changed the same way. Only the images, their views and
    /// framebuffers are rebuilt, after the device is idle - the render pass,
    /// the sampler and the casters' pipelines do not depend on the size, the
    /// viewports are set when the pass is recorded, and the shaders read the
    /// tiles and their texel sizes from the per-frame block each frame.
    uint32_t SHADOW_MAP_SIZE = 4096;
    /// Cascades: concentric squares centred on the player, each its own tile
    /// of one depth atlas. One is the single map as it always was, two (the
    /// default) put a fine tile over the ground the camera is near, three
    /// split that once more. See computeLightSpaceMatrix and shadow_csm.glsli.
    static constexpr int kMaxShadowCascades = 3;
    /// How many cascades the atlas is built for now.
    int shadowCascadeCount_ = 2;
    /// What the shadow cascades setting last asked for, built or not.
    int shadowCascadesWanted_ = 2;
    /// The resolution setting's step, kept so a cascade change can work the
    /// tile out again from it.
    int shadowResolutionStep_ = 0;
    /// A cascade count asked for and not built yet; 0 when nothing is
    /// waiting. Goes with pendingShadowMapSize_.
    int pendingShadowCascades_ = 0;
    /// The atlas the tiles are laid out in: N by N for one cascade, 2N by N
    /// for two and 2N by 2N for three, the fourth quarter unused.
    static uint32_t shadowAtlasWidth(int cascades, uint32_t tile) { return cascades > 1 ? tile * 2 : tile; }
    static uint32_t shadowAtlasHeight(int cascades, uint32_t tile) { return cascades > 2 ? tile * 2 : tile; }
    /// The largest tile allowed with two or more cascades: an 8192 tile is
    /// already a 16384 by 8192 atlas, 512 MB, twice over for the frames in
    /// flight.
    static constexpr uint32_t kMaxCascadeTileSize = 8192;
    /// One cascade as this frame draws and reads it.
    struct ShadowCascade {
        glm::mat4 lightSpace{0.0f};  ///< world to the cascade's clip space, what the casters draw with
        glm::mat4 atlas{0.0f};       ///< world to atlas UV and depth, what the shaders read with
        glm::vec3 center{0.0f};      ///< texel-snapped, as the casters cull around it
        float halfExtent = 0.0f;     ///< yards from the centre to a side
        float texelWorld = 0.0f;     ///< one texel of its tile, in yards
        VkRect2D tile{};             ///< where it is drawn in the atlas
    };
    ShadowCascade shadowCascades_[kMaxShadowCascades];
    /// How many of shadowCascades_ this frame filled; 0 before the player has
    /// a position, when the shadow pass is skipped.
    int activeShadowCascades_ = 0;
    /// Works out the tile for the wanted cascade count and resolution step
    /// and leaves it for the next frame to build.
    void requestShadowAtlas();
    /// The quality level's side, and the smallest the resolution setting
    /// offers: its steps are this, twice, four and eight times it.
    uint32_t shadowMapBaseSize_ = 4096;
    /// setTextureMagnification's mode, handed to the shaders in viewPos.w.
    int textureMagnification_ = 1;
    /// A side asked for and not built yet; 0 when nothing is waiting.
    uint32_t pendingShadowMapSize_ = 0;
    /// The largest side the resolution setting goes to. Each doubling is four
    /// times the memory: at 16384 a map is 1 GB, two in flight are 2 GB,
    /// and past that is more than a player choosing "sharper" expects to pay.
    static constexpr uint32_t kMaxShadowMapSize = 16384;
    void setShadowMapSize(uint32_t side) {
        // Powers of two between 512 and 4096: the quality slider has five
        // steps and these are they.
        shadowMapBaseSize_ = std::clamp(side, 512u, 4096u);
        SHADOW_MAP_SIZE = shadowMapBaseSize_;
    }
    /// The largest tile side for this many cascades: kMaxShadowMapSize for
    /// one and kMaxCascadeTileSize for more, or less where the device cannot
    /// make or draw into an atlas that large.
    uint32_t maxShadowMapSize(int cascades) const;
    /// The per-frame-slot atlases, views and framebuffers, for
    /// shadowCascadeCount_ tiles of SHADOW_MAP_SIZE. shadowRenderPass has to
    /// exist first. On failure what was made is left for
    /// destroyShadowMapImages.
    bool createShadowMapImages();
    void destroyShadowMapImages();
    /// Binding 1 of every per-frame set, and the fog's own, after the views
    /// have been made again.
    void writeShadowMapBindings();
    /// Rebuilds the atlas at pendingShadowMapSize_ and pendingShadowCascades_.
    /// Between frames only.
    void applyPendingShadowMapSize();
    // Per-frame shadow resources: each in-flight frame has its own depth image and
    // framebuffer so that frame N's shadow read and frame N+1's shadow write don't
    // race on the same image across concurrent GPU submissions.
    // Array size must match MAX_FRAMES (= 2, defined in the private section below).
    VkImage shadowDepthImage[2] = {};
    VmaAllocation shadowDepthAlloc[2] = {};
    VkImageView shadowDepthView[2] = {};
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer[2] = {};
    VkImageLayout shadowDepthLayout_[2] = {};
    /// The outer cascade's clip-space matrix, zero before the player has a
    /// position; the cascades themselves are in shadowCascades_.
    glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
    /// The outer cascade's centre.
    glm::vec3 shadowCenter = glm::vec3(0.0f);
    bool shadowCenterInitialized = false;
    bool shadowsEnabled = false;
    float shadowDistance_ = 300.0f;  // Shadow frustum half-extent (default: 300 units)
    float viewDistance_ = 1200.0f;
    /// The far clip the world is drawn to: the setting held to this map's
    /// limit (daynight::clientFarClip, 0x00780770), and the map it was worked
    /// out for, so a map change reapplies it.
    float farClip_ = 1200.0f;
    uint32_t farClipMapId_ = 0;
    /// The last SMSG_WEATHER handed to the weather (GameHandler::getWeatherSerial).
    uint32_t weatherSerialSeen_ = 0;
    /// Weather::lightValue as this frame's lighting took it.
    float weatherLight_ = 0.0f;
    void applyFarClip(uint32_t mapId);
    bool sharpStars_ = false;
    float diagTerrainFurthest_ = -1.0f;
    float diagM2Furthest_ = -1.0f;


public:
    // Character preview registration (for off-screen composite pass)
    void registerPreview(CharacterPreview* preview);
    void unregisterPreview(CharacterPreview* preview);

    /// Whether the lighting reads the shadow map. Off, as the client's
    /// extShadowQuality 0 is, the casters are still drawn - skipping them is
    /// the path that lost the device - and only the shaders stop applying it.
    void setShadowsEnabled(bool enabled) { shadowsEnabled = enabled; }
    bool areShadowsEnabled() const { return shadowsEnabled; }
    void setShadowDistance(float dist) { shadowDistance_ = glm::clamp(dist, 40.0f, 500.0f); }
    float getShadowDistance() const { return shadowDistance_; }
    /// The shadow map's side as a step above the quality level's: 0 is that
    /// size, 1 twice it and 2 four times it, held to maxShadowMapSize().
    /// Applied at the start of the next frame. With cascades the step is the
    /// side of each cascade's tile, held to kMaxCascadeTileSize.
    void setShadowResolutionScale(int step);
    /// The way sunlight travels for the shadow map and the mist's shafts:
    /// from the sun as the sky draws it while it is up, the light's own
    /// direction otherwise.
    [[nodiscard]] glm::vec3 sunTravelDirection() const;
    /// How many shadow cascades, 1 to kMaxShadowCascades. Rebuilds the atlas
    /// at the start of the next frame, as the resolution does.
    void setShadowCascadeCount(int count);
    int getShadowCascadeCount() const { return shadowCascadeCount_; }
    /// How a magnified texture is filtered: 0 bilinear (the client's),
    /// 1 cubic B-spline. See texture_filter.glsli.
    void setTextureMagnification(int mode) { textureMagnification_ = std::clamp(mode, 0, 1); }
    uint32_t getShadowMapSize() const { return SHADOW_MAP_SIZE; }
    void setViewDistance(float distance);
    float getViewDistance() const { return viewDistance_; }
    /// What the world is actually drawn to on this map: the view distance
    /// held to 791 yards before map 530 and 1583 from it (0x00780770).
    float getFarClip() const { return farClip_; }
    /// Draw the client's own point stars in place of the sky model's baked
    /// star layer, which is a 256x256 compressed texture stretched across the
    /// whole dome. See Renderer::setSharpStars.
    /// WOWEE_VIEW_DIAG=1: one line naming how far terrain and doodads each
    /// actually drew, so the two can be compared rather than reasoned about.
    void logViewDistanceDiag();
    void setSharpStars(bool enabled);
    bool areSharpStars() const { return sharpStars_; }
    /// Fog lit by the sun through the shadow map and by nearby torches: 0 is
    /// off, 1-3 the volume's resolution. Applied at the start of the next
    /// frame. See VolumetricFog.
    void setVolumetricFogQuality(int quality);
    /// Ray traced lighting: 0 off, 1 sun shadows, 2 + ambient occlusion,
    /// 3 + one-bounce diffuse. See RtLighting.
    void setRtLightingMode(int mode);
    /// A multiplier on how thick that air is; 1 is the default mist.
    void setVolumetricFogDensity(float density) { volumetricFogDensity_ = glm::clamp(density, 0.0f, 3.0f); }
    /// Rays streaming from the sun across the finished picture. See SunShafts.
    void setSunShaftsEnabled(bool enabled) { sunShaftsEnabled_ = enabled; }
    /// The client's full-screen glow, ffxGlow (on by default where shaders
    /// are supported). See ScreenEffects.
    void setScreenGlowEnabled(bool enabled) { screenGlowEnabled_ = enabled; }
    int getTerrainLoadRadius() const;
    int getTerrainUnloadRadius() const { return getTerrainLoadRadius() + 3; }
    void setMsaaSamples(VkSampleCountFlagBits samples);

    // Post-process pipeline API - delegates to PostProcessPipeline (§4.3)
    PostProcessPipeline* getPostProcessPipeline() const;
    void setFSREnabled(bool enabled);
    void setFSR2Enabled(bool enabled);

    /// Off (the default): the client's own liquid (WaterRenderer's client
    /// path). On: this client's procedural water, with its reflection pass
    /// and refraction copy.
    void setEnhancedWaterEnabled(bool enabled);
    [[nodiscard]] bool isEnhancedWaterEnabled() const { return enhancedWater_; }
    /// The client's 'specular' option (0x0078de60 sets world flag 0x8000000;
    /// off by default): water drawn as psLiquidWater rather than NoSpec, the
    /// terrain's "_s.blp" textures and highlight. As in the client it takes
    /// effect "on restart": the value is latched when the terrain and water
    /// renderers are made, and a later change waits for the next start.
    void setClientSpecular(bool on) { pendingClientSpecular_ = on; }
    [[nodiscard]] bool clientSpecular() const { return clientSpecular_; }

private:
    void applyMsaaChange();
    bool updateSkyboxLayers();
    uint32_t loadSkyboxModel(const std::string& path);
    /// Reads, parses and uploads one sky model into `target`; 0 on failure.
    uint32_t uploadSkyModel(M2Renderer& target, const std::string& path);
    /// The stars model at the eye, faded by the client's star clock.
    void updateStarsModel(float deltaTime);
    VkSampleCountFlagBits pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    bool msaaChangePending_ = false;
    void renderShadowPass();
    /// Whether this frame's shadow pass draws this slot's map and leaves it
    /// readable. The volumetric fog samples that map, so it runs by the same
    /// answer. Valid after computeLightSpaceMatrix for the frame.
    [[nodiscard]] bool shadowPassDrawsThisFrame() const;
    glm::mat4 computeLightSpaceMatrix();

    std::vector<pipeline::CustomZoneInfo> customZones_;
    pipeline::AssetManager* cachedAssetManager = nullptr;

    // Spell visual effects - owned SpellVisualSystem (extracted from Renderer §4.4)
    std::unique_ptr<SpellVisualSystem> spellVisualSystem_;

    // Post-process pipeline - owns all FSR/FXAA/FSR2 state (extracted §4.3)
    std::unique_ptr<PostProcessPipeline> postProcessPipeline_;

    bool playerIndoors_ = false;  // Cached WMO inside state for macro conditionals
    bool deferredWorldInitEnabled_ = true;
    bool deferredWorldInitPending_ = false;
    uint8_t deferredWorldInitStage_ = 0;
    float deferredWorldInitCooldown_ = 0.0f;

    // Third-person character state
    glm::vec3 characterPosition = glm::vec3(0.0f);
    uint32_t characterInstanceId = 0;
    float characterYaw = 0.0f;

    // Where the player was, for shaders that react to them moving through the
    // world. playerWakePos_ chases characterPosition with a fixed time
    // constant; the lag is what gives brushed-past grass its springback.
    glm::vec3 playerWakePos_ = glm::vec3(0.0f);
    glm::vec3 prevPlayerPos_ = glm::vec3(0.0f);
    float playerSpeed_ = 0.0f;
    bool playerMotionTracked_ = false;



    // Selection circle + overlay rendering (owned by OverlaySystem)
    std::unique_ptr<OverlaySystem> overlaySystem_;



    // Vulkan frame state
    VkContext* vkCtx = nullptr;
    VkCommandBuffer currentCmd = VK_NULL_HANDLE;
    uint32_t currentImageIndex = 0;

    // Per-frame UBO + descriptors (set 0)
    static constexpr uint32_t MAX_FRAMES = 2;
    VkDescriptorSetLayout perFrameSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet perFrameDescSets[MAX_FRAMES] = {};
    VkBuffer perFrameUBOs[MAX_FRAMES] = {};
    VmaAllocation perFrameUBOAllocs[MAX_FRAMES] = {};
    void* perFrameUBOMapped[MAX_FRAMES] = {};
    GPUPerFrameData currentFrameData{};
    float globalTime = 0.0f;

    // Per-frame reflection UBO (mirrors camera for planar reflections)
    VkBuffer reflPerFrameUBO = VK_NULL_HANDLE;
    VmaAllocation reflPerFrameUBOAlloc = VK_NULL_HANDLE;
    void* reflPerFrameUBOMapped = nullptr;
    VkDescriptorSet reflPerFrameDescSet[MAX_FRAMES] = {};

    bool createPerFrameResources();
    void destroyPerFrameResources();
    void updatePerFrameUBO();
    void setupWater1xPass();
    void renderReflectionPass();

    // ── Multithreaded secondary command buffer recording ──
    // Indices into secondaryCmds_ arrays
    static constexpr uint32_t SEC_SKY       = 0;  // sky (main thread)
    static constexpr uint32_t SEC_TERRAIN   = 1;  // terrain (worker 0)
    static constexpr uint32_t SEC_WMO       = 2;  // WMO (worker 1)
    static constexpr uint32_t SEC_SELECTION = 3;  // selection circle (main thread)
    static constexpr uint32_t SEC_CHARS     = 4;  // characters (worker 2)
    static constexpr uint32_t SEC_M2        = 5;  // M2 + particles + glow (worker 3)
    static constexpr uint32_t SEC_POST      = 6;  // water + weather + effects (worker 4)
    static constexpr uint32_t SEC_IMGUI     = 7;  // ImGui (main thread, non-FSR only)
    static constexpr uint32_t NUM_SECONDARIES = 8;
    static constexpr uint32_t NUM_WORKERS = 5;

    /// The doodads and, among their blended batches, the characters' own, by
    /// distance. withCharacters: whether the characters draw at all.
    void renderM2Models(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, bool withCharacters);

    // Per-worker command pools (thread-safe: one pool per thread)
    VkCommandPool workerCmdPools_[NUM_WORKERS] = {};
    // Main-thread command pool for its secondary buffers
    VkCommandPool mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    // Pre-allocated secondary command buffers [secondaryIndex][frameInFlight]
    VkCommandBuffer secondaryCmds_[NUM_SECONDARIES][MAX_FRAMES] = {};

    bool parallelRecordingEnabled_ = false;  // set true after pools/buffers created
    // WOWEE_PASS_ABLATION: switches one world pass off at a time and reports
    // what the frame did without it. See pass_ablation.hpp for why the GPU's
    // own timestamps cannot answer that on this platform.
    std::unique_ptr<PassAblation> passAblation_;
    bool passAblationReported_ = false;
    bool worldDrawnLastFrame_ = false;
    std::chrono::steady_clock::time_point lastFrameStart_{};
    float lastDeltaTime_ = 0.0f;           // cached for post-process pipeline
    bool createSecondaryCommandResources();
    void destroySecondaryCommandResources();
    VkCommandBuffer beginSecondary(uint32_t secondaryIndex);
    void setSecondaryViewportScissor(VkCommandBuffer cmd);

    // Cached render pass state for secondary buffer inheritance
    VkRenderPass activeRenderPass_ = VK_NULL_HANDLE;
    VkFramebuffer activeFramebuffer_ = VK_NULL_HANDLE;
    VkExtent2D activeRenderExtent_ = {.width = 0, .height = 0};

    // Active character previews for off-screen rendering
    std::vector<CharacterPreview*> activePreviews_;

    bool terrainEnabled = true;
    bool terrainLoaded = false;

    /// extShadowQuality as read at start-up (0-4); below 2 the terrain's
    /// baked shadow dims a world object's direct light (0x007a1bc0).
    int extShadowQuality_ = 0;
    bool ghostMode_ = false;  // set each frame from gameHandler->isPlayerGhost()

    // Render Graph - declarative pass ordering with automatic barriers
    std::unique_ptr<RenderGraph> renderGraph_;
    void buildFrameGraph(game::GameHandler* gameHandler);

    // HiZ occlusion culling - builds depth pyramid each frame
    std::unique_ptr<HiZSystem> hizSystem_;

    // Volumetric fog: a froxel volume built after the shadow pass and read by
    // every world shader through set 0 binding 2.
    std::unique_ptr<VolumetricFog> volumetricFog_;
    // Ray traced lighting. Created with the per-frame resources and destroyed
    // after every renderer that registers geometry with the scene.
    std::unique_ptr<RtScene> rtScene_;
    std::unique_ptr<RtLighting> rtLighting_;
    void writeRtLightingBindings();
    VkExtent2D sceneRenderExtent() const;
    void recordRtLighting(VkImage sceneDepth, VkExtent2D sceneExtent, bool depthIsMsaa);
    bool rtRecordedThisFrame_ = false;
    float volumetricFogDensity_ = 1.0f;
    /// Whether this frame builds the volume, decided where the per-frame block
    /// is written so the block's switch and the dispatch cannot disagree.
    bool volumetricThisFrame_ = false;
    /// The ground the mist lies on, chased toward the ground under the player.
    float fogLayerBase_ = 0.0f;
    bool fogLayerBaseValid_ = false;
    /// The extinction the volume is built with, chased toward what the zone,
    /// the weather and the hour ask for so walking indoors does not switch it.
    float fogExtinction_ = -1.0f;
    void renderVolumetricFog();
    void writeFogVolumeBindings();
    float volumetricFogExtinction() const;

    // Screen recording: the GPU side reads frames back, the recorder encodes.
    std::unique_ptr<ScreenCapture> screenCapture_;
    std::unique_ptr<core::ScreenRecorder> recorder_;
    std::string recordingFailure_;
    void collectRecordedFrame();
    void recordScreenCapture();

    // Screen-space sun shafts, built from the finished frame at the end of
    // endFrame and added in the overlay pass ahead of the interface.
    std::unique_ptr<SunShafts> sunShafts_;
    bool sunShaftsEnabled_ = false;  // opt-in: the original client has none
    bool enhancedWater_ = false;     // opt-in: the original client draws its own liquid
    bool pendingClientSpecular_ = false;  // the 'specular' setting as last set
    bool clientSpecular_ = false;         // ...as latched at the first world load
    /// renderWorld ran this frame. The shafts are built from the world's
    /// picture, and a login screen or a loading screen is not one.
    bool worldDrawnThisFrame_ = false;
    void recordSunShafts();

    // The client's glow and death passes (ffxGlow, ffxDeath), built from the
    // finished frame like the shafts and drawn in the overlay pass under them.
    std::unique_ptr<ScreenEffects> screenEffects_;
    bool screenGlowEnabled_ = true;
    void recordScreenEffects();

    // GPU-driven grass: compute cull with atomic compaction feeding an
    // indirect draw, over a population generated from terrain suitability.
    std::unique_ptr<GrassRenderer> grassRenderer_;
    // Where the live population was generated for. Rebuilt when the player
    // leaves it; the generator's lattice is world-anchored, so a rebuild
    // reproduces every blade that is still in range rather than reshuffling.
    glm::vec3 grassWindowCenter_{0.0f};
    bool grassWindowValid_ = false;
    // The window being generated, a bounded number of lattice cells per
    // frame. Small windows still finish inside one; the ones the distance
    // slider allows take as many frames as they take, and upload when done.
    pipeline::GrassPopulationBuilder grassBuilder_;
    glm::vec3 grassBuildCenter_{0.0f};
    // Clearings around placed WMOs and props for the window being built,
    // gathered once per rebuild from the renderers that own the instances.
    pipeline::GrassClearingField grassClearing_;
    void updateGrassPopulation();
public:
    /** Grass density and height, as fractions of the generator's defaults.
     * Set from the settings panel; either changing rebuilds the field. */
    void setGrassScales(float density, float height);
    /** How far out grass draws, in yards. Density thins with distance past
     * the near field, so range costs blades logarithmically, not by area. */
    void setGrassDistance(float yards);
    /** Turn grass on or off. Off is the default: it is new, it costs
     * generation time on the main thread, and turning it off has to release
     * what it was holding rather than merely stop drawing. */
    void setGrassEnabled(bool enabled);
private:
    bool grassEnabled_ = false;
    float grassDensityScale_ = 1.0f;
    float grassHeightScale_ = 1.0f;
    float grassDistance_ = 150.0f;
    // (biome, effectId) -> index into the profile table, built as effects are
    // met. Grass profiles are a blend of five categories crossed with the
    // biome overrides, so distinct ones stay far fewer than the hundreds of
    // ground effects that map onto them.
    std::unordered_map<uint64_t, uint32_t> grassProfileIndex_;
    std::vector<pipeline::GrassProfile> grassProfiles_;
    uint32_t grassProfileFor(uint32_t effectId, uint32_t areaId);
    // The per-zone look table from assets/grass_biomes.json, loaded on first
    // use, and a memo of which biome each area resolved to - resolution walks
    // AreaTable parentage and runs per blade sample without it.
    pipeline::GrassBiomeSet grassBiomes_;
    bool grassBiomesLoaded_ = false;
    std::unordered_map<uint32_t, uint32_t> grassBiomeForArea_;
    // Session totals, reported once at shutdown. The log is bounded and the
    // per-rebuild lines rotate out of it whenever the player stands still for
    // a minute, so the tail is the only place a "check the log" can rely on.
    uint32_t grassRebuilds_ = 0;
    size_t grassLastCount_ = 0;
    double grassWorstGenerateMs_ = 0.0;

    // CPU timing stats (last frame/update).
    double lastUpdateMs = 0.0;
    double lastRenderMs = 0.0;
    double lastCameraUpdateMs = 0.0;
    double lastTerrainRenderMs = 0.0;
    double lastWMORenderMs = 0.0;
    double lastM2RenderMs = 0.0;
};

} // namespace rendering
} // namespace wowee
