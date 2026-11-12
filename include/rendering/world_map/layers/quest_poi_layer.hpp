// quest_poi_layer.hpp - Quest objective markers on the world map.
#pragma once
#include "rendering/world_map/overlay_renderer.hpp"
#include "rendering/world_map/world_map_types.hpp"
#include "rendering/imgui_texture.hpp"
#include <string>
#include <vector>

namespace wowee {
namespace rendering {
class VkContext;
}
namespace pipeline { class AssetManager; }
namespace rendering {
namespace world_map {

class QuestPOILayer : public IOverlayLayer {
public:
    ~QuestPOILayer() override;
    void initialize(VkContext* ctx, pipeline::AssetManager* am);
    void setPois(const std::vector<QuestPOI>& pois) { pois_ = &pois; }
    /// WorldMapBlobFrame's look, as the interface sets it: fill and border
    /// textures (none: this client's own flat wash), their alphas, and the
    /// border's width in hundredths of the screen.
    void setBlobStyle(const std::string& fill, uint8_t fillAlpha, const std::string& border,
                      uint8_t borderAlpha, float borderScalar);
    void render(const LayerContext& ctx) override;
private:
    struct Loaded {
        std::string path;
        ImGuiTexture texture;
        bool attempted = false;
    };
    ImTextureID textureFor(Loaded& slot, const std::string& path);

    const std::vector<QuestPOI>* pois_ = nullptr;
    VkContext* vkCtx_ = nullptr;
    pipeline::AssetManager* assetManager_ = nullptr;
    Loaded fill_, border_;
    std::string fillPath_, borderPath_;
    uint8_t fillAlpha_ = 255, borderAlpha_ = 255;
    float borderScalar_ = 1.0f;
};

} // namespace world_map
} // namespace rendering
} // namespace wowee
