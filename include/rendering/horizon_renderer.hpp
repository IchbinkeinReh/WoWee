#pragma once

#include "rendering/vk_utils.hpp"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
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
///   already covers.
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

    /// Draws the tiles in reach. farClip: the client's far clip; scale:
    /// horizonFarclipScale.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera, float farClip,
                float scale);

    [[nodiscard]] uint32_t tilesDrawn() const { return tilesDrawn_; }

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
};

}  // namespace rendering
}  // namespace wowee
