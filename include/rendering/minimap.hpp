#pragma once

#include "rendering/minimap_indoor.hpp"
#include "rendering/minimap_zoom.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <deque>
#include <algorithm>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;
class VkTexture;
class VkRenderTarget;

class Minimap {
public:
    Minimap();
    ~Minimap();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, int size = 200);
    void shutdown();
    void recreatePipelines();
    /// The display pipeline, which initialize() and recreatePipelines()
    /// both build.
    void buildDisplayPipeline(VkDevice device,
                              const VkPipelineShaderStageCreateInfo& vertStage,
                              const VkPipelineShaderStageCreateInfo& fragStage);

    /// Select the render pass the display quad is recorded into.
    ///
    /// The minimap draws over the finished scene, and water is the one thing
    /// that draws after it: where water leaves the scene pass for a
    /// continuation of its own, the minimap has to follow or the water sheet is
    /// painted straight over it. Same reason SwimEffects has this.
    ///
    /// A null pass means the scene pass, which is where it goes when water has
    /// not moved. Call before initialize() or recreatePipelines().
    void setTargetPass(VkRenderPass pass, VkSampleCountFlagBits samples) {
        targetPass_ = pass;
        targetSamples_ = samples;
    }

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }
    void setMapName(const std::string& name);

    /// Off-screen composite pass - call BEFORE the main render pass begins.
    void compositePass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos);

    /// Display quad - call INSIDE the main render pass.
    void render(VkCommandBuffer cmd, const Camera& playerCamera,
                const glm::vec3& centerWorldPos, int screenWidth, int screenHeight);

    void setEnabled(bool enabled) { this->enabled = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled; }
    void toggle() { enabled = !enabled; }

    void setRotateWithCamera(bool rotate) { rotateWithCamera = rotate; }
    [[nodiscard]] bool isRotateWithCamera() const { return rotateWithCamera; }

    void setSquareShape(bool square) { squareShape = square; }
    [[nodiscard]] bool isSquareShape() const { return squareShape; }
    /// How far the map reaches from the player, in yards, at the zoom it is
    /// at (0x007f3b90: the indoor table inside).
    [[nodiscard]] float getViewRadius() const {
        return minimap_zoom::radius(getZoomLevel(), isIndoors());
    }

    /// One of the client's six levels (minimap_zoom), 0 furthest out. The
    /// client keeps one for outside and one for inside (minimapZoom and
    /// minimapInsideZoom) and these set and read the one in use, as
    /// Minimap:SetZoom and GetZoom do (0x007f3ae0, 0x007f3b40).
    void setZoomLevel(int level) {
        (isIndoors() ? insideZoomLevel_ : zoomLevel_) = minimap_zoom::clampLevel(level);
    }
    [[nodiscard]] int getZoomLevel() const {
        return isIndoors() ? insideZoomLevel_ : zoomLevel_;
    }

    /// The building the player is inside, which makes this the indoor map -
    /// drawn from the WMO groups' own pictures, at the inside zoom - or none
    /// outdoors (minimap_indoor.hpp). Set every frame, before compositePass.
    void setIndoorScene(std::optional<minimap_indoor::Scene> scene);
    [[nodiscard]] bool isIndoors() const { return indoorScene_.has_value(); }
    /// The reach the indoor map would have, for finding the groups that make
    /// it up before it is known to be indoors.
    [[nodiscard]] float insideViewRadius() const {
        return minimap_zoom::radius(insideZoomLevel_, true);
    }
    void zoomIn() { if (getZoomLevel() < minimap_zoom::kLevels - 1) setZoomLevel(getZoomLevel() + 1); }
    void zoomOut() { if (getZoomLevel() > 0) setZoomLevel(getZoomLevel() - 1); }

    void setOpacity(float opacity) { opacity_ = opacity; }

    /// Where the map is drawn, in pixels from the top-left of the window.
    ///
    /// Unset, it goes in the top-right corner at its own size, which is where
    /// this client's own interface puts it. FrameXML puts it inside a frame it
    /// owns, so when the original interface is drawing the minimap the rect of
    /// that frame is handed here instead - this is a Vulkan pass of its own
    /// rather than an image the widget renderer could draw, so the map moves to
    /// the frame rather than the frame receiving the map.
    void setScreenRect(float x, float y, float w, float h) {
        rectX_ = x; rectY_ = y; rectW_ = w; rectH_ = h; haveRect_ = true;
    }
    void clearScreenRect() { haveRect_ = false; }
    /// Where the map is actually being drawn, when something placed it.
    ///
    /// The marker pass needs this: it used to assume the corner this client
    /// puts its own minimap in, and when FrameXML draws the ring the map moves
    /// to whatever rect the Minimap widget occupies. Blips computed against
    /// the old corner land beside the map rather than on it.
    [[nodiscard]] bool hasScreenRect() const { return haveRect_; }
    /// Where render() puts the map, as a fraction of the window (x, y, w, h).
    [[nodiscard]] glm::vec4 screenUvRect(int screenWidth, int screenHeight) const;
    [[nodiscard]] float screenRectX() const { return rectX_; }
    [[nodiscard]] float screenRectY() const { return rectY_; }
    [[nodiscard]] float screenRectW() const { return rectW_; }
    [[nodiscard]] float screenRectH() const { return rectH_; }

    // Public accessors for WorldMap
    VkTexture* getOrLoadTileTexture(int tileX, int tileY);
    void ensureTRSParsed() { if (!trsParsed) parseTRS(); }
    [[nodiscard]] const std::string& getMapName() const { return mapName; }

private:
    void parseTRS();
    /// A minimap texture by its md5translate name ("Azeroth\\map32_49",
    /// without .blp), or null when the table or the archive has none.
    VkTexture* loadTrsTexture(const std::string& key);
    void updateTileDescriptors(uint32_t frameIdx, int centerTileX, int centerTileY);

    VkContext* vkCtx = nullptr;
    /// Null until something says otherwise - see setTargetPass.
    VkRenderPass targetPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits targetSamples_ = VK_SAMPLE_COUNT_1_BIT;
    pipeline::AssetManager* assetManager = nullptr;
    std::string mapName = "Azeroth";

    // TRS lookup: "Azeroth\map32_49" → "e7f0dea73ee6baca78231aaf4b7e772a"
    std::unordered_map<std::string, std::string> trsLookup;
    bool trsParsed = false;

    // Tile texture cache: hash → VkTexture
    // Evicted (FIFO) when the count of successfully-loaded tiles exceeds MAX_TILE_CACHE.
    static constexpr size_t MAX_TILE_CACHE = 128;
    std::unordered_map<std::string, std::unique_ptr<VkTexture>> tileTextureCache;
    std::deque<std::string> tileInsertionOrder;  // hashes of successfully loaded tiles, oldest first
    std::unique_ptr<VkTexture> noDataTexture;

    // Composite render target (GRID x GRID tiles)
    std::unique_ptr<VkRenderTarget> compositeTarget;
    static constexpr int TILE_PX = 256;
    // The composite is a square of GRID x GRID tiles around the player's own.
    // The furthest zoom reaches 233 yards each way (minimap_zoom) and the
    // player can stand anywhere in the middle tile, so one tile (533 yards)
    // past it on every side is enough.
    static constexpr int GRID = 3;
    static constexpr int COMPOSITE_PX = TILE_PX * GRID;

    // Shared quad vertex buffer (6 verts, pos2 + uv2 = 16 bytes/vert)
    ::VkBuffer quadVB = VK_NULL_HANDLE;
    VmaAllocation quadVBAlloc = VK_NULL_HANDLE;

    // Descriptor resources (shared layout: 1 combined image sampler at binding 0)
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_DESC_SETS = 2 * GRID * GRID + 1;

    // Tile composite pipeline (renders into VkRenderTarget)
    VkPipeline tilePipeline = VK_NULL_HANDLE;
    VkPipelineLayout tilePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet tileDescSets[2][GRID * GRID] = {};  // [frameInFlight][tileSlot]

    // Display pipeline (renders into main render pass)
    VkPipeline displayPipeline = VK_NULL_HANDLE;
    VkPipelineLayout displayPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet displayDescSet = VK_NULL_HANDLE;

    int mapSize = 200;
    int zoomLevel_ = minimap_zoom::kDefaultLevel;
    int insideZoomLevel_ = minimap_zoom::kDefaultLevel;

    // Indoors (setIndoorScene). The composite is then the area about the
    // player rather than the terrain tiles, and compositeCenter_ and
    // compositeSpan_ say where it is, in render axes.
    std::optional<minimap_indoor::Scene> indoorScene_;
    bool indoorDirty_ = false;
    bool compositeIndoors_ = false;
    glm::vec2 compositeCenter_{0.0f};
    float compositeSpan_ = 1.0f;
    /// The client draws at most 256 pictures (0x007f5ba0's 0x100).
    static constexpr uint32_t MAX_INDOOR_TILES = 256;
    VkPipeline indoorPipeline = VK_NULL_HANDLE;
    VkPipelineLayout indoorPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool indoorDescPool = VK_NULL_HANDLE;
    VkDescriptorSet indoorDescSets[2][MAX_INDOOR_TILES] = {};
    void compositeIndoor(VkCommandBuffer cmd);
    bool enabled = true;
    bool rotateWithCamera = false;
    bool squareShape = false;
    float opacity_ = 1.0f;
    bool  haveRect_ = false;
    float rectX_ = 0.0f, rectY_ = 0.0f, rectW_ = 0.0f, rectH_ = 0.0f;

    // Throttling
    float updateIntervalSec = 0.25f;
    float updateDistance = 6.0f;
    std::chrono::steady_clock::time_point lastUpdateTime{};
    glm::vec3 lastUpdatePos{0.0f};
    bool hasCachedFrame = false;

    // Tile tracking
    int lastCenterTileX = -1;
    int lastCenterTileY = -1;

    // No arrow here: the player's arrow is the interface's MinimapArrow,
    // drawn with the blips (GameScreen::renderMinimapPlayerArrow).
};

} // namespace rendering
} // namespace wowee
