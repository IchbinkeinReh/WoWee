#pragma once

#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;

/// The horizon past the far clip: the map's .wdl heights, drawn as the client
/// draws them (CMapLowDetail, 0x007cc0b0 by way of 0x00795f80).
///
/// - Each tile within horizonFarclipScale (3..6) x the far clip of the
///   camera, in its frustum, is drawn whole (0x007d5e70): 545 vertices, 4
///   triangles a cell.
/// - Fogged through: fog from 0 to 1 yard in the zone's fog colour, so a
///   tile is a silhouette in the fog colour (render states 8, 9, 10, 12).
/// - In a projection of its own (0x00791170): near the far clip less 50,
///   far the scale x the far clip; the near plane takes away what the ground
///   already covers. A tile whose ground is not in is drawn from just in
///   front of the camera (pipeline::wdl::tileNear), so no hole opens where
///   the ground has yet to load or is not loaded; one reaching out of the
///   far clip's square the ground's chunks are drawn in is drawn from there
///   too and gives way to the ground inside that square
///   (pipeline::wdl::tileDraw), so none opens where the square's side comes
///   in front of the far clip at the corners of a wide or diagonal view.
/// - At depths behind the whole world and before the sky's (the client's
///   viewport depth range 0.998..0.999 against the world's 0..0.94): the
///   world always stands in front of it, it in front of the sky.
class HorizonRenderer {
public:
    HorizonRenderer() = default;
    ~HorizonRenderer();
    HorizonRenderer(const HorizonRenderer&) = delete;
    HorizonRenderer& operator=(const HorizonRenderer&) = delete;

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    void shutdown();
    void recreatePipelines();

    /// The map to draw; its .wdl is read on the next update.
    void setMapName(const std::string& name);
    /// Main thread, before the frame is recorded: loads a new map's .wdl.
    void update(pipeline::AssetManager* assets);

    /// Whether the ground of the tile at (row, column) - ADT file index
    /// (column, row) - is drawn.
    using GroundIn = std::function<bool(int row, int col)>;

    /// Draws the tiles in reach. farClip: the client's far clip; scale:
    /// horizonFarclipScale; groundIn: which tiles' ground is in (none: all).
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera, float farClip,
                float scale, const GroundIn& groundIn = {});

    [[nodiscard]] uint32_t tilesDrawn() const { return tilesDrawn_; }
    /// The tiles the last frame drew, as row * 64 + column; those drawn from
    /// in front of the camera for want of their ground (wdl::tileNear) have
    /// 0x8000 set; those standing in for the ground past the side of the
    /// square its chunks are drawn in (wdl::tileDraw) 0x4000.
    [[nodiscard]] const std::vector<uint16_t>& drawnTiles() const { return drawnTiles_; }

private:
    void buildPipeline();
    void releaseMesh();

    VkContext* vkCtx_ = nullptr;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    std::string mapName_;
    std::string loadedMap_;
    AllocatedBuffer vertices_{};
    AllocatedBuffer indices_{};
    /// Per present tile: its row/col and its box.
    struct Tile {
        int row = 0, col = 0;
        float minZ = 0.0f, maxZ = 0.0f;
    };
    std::vector<Tile> tiles_;
    /// MAOF slot -> index into tiles_ (and the vertex buffer), -1 for none.
    std::array<int32_t, 64 * 64> slot_{};
    uint32_t tilesDrawn_ = 0;
    std::vector<uint16_t> drawnTiles_;
};

}  // namespace rendering
}  // namespace wowee
