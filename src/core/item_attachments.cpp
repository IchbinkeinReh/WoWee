#include "core/item_attachments.hpp"

#include "core/weapon_attachment.hpp"
#include "core/helm_visual.hpp"
#include "core/asset_prefetch.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/character_renderer.hpp"

#include <string>

namespace wowee {
namespace core {

namespace {

/// A prepared model if the prefetch has one, else read here.
bool itemModel(pipeline::AssetManager& assets, const AssetPrefetch* prefetch, const std::string& path,
               pipeline::M2Model& out) {
    if (prefetch) {
        if (auto prepared = prefetch->model(path)) {
            out = *prepared;
            return out.isValid();
        }
    }
    return pipeline::loadM2WithSkin(assets, path, out) && out.isValid();
}

struct ShoulderPiece {
    std::string modelPath;
    std::string texturePath;
};

/// A shoulder display's two models, as the row names them.
std::vector<ShoulderPiece> shoulderPieces(pipeline::AssetManager& assets, uint32_t displayInfoId) {
    std::vector<ShoulderPiece> pieces;
    if (displayInfoId == 0) return pieces;
    auto dbc = assets.loadDBC("ItemDisplayInfo.dbc");
    if (!dbc || !dbc->isLoaded()) return pieces;
    const int32_t row = dbc->findRecordById(displayInfoId);
    if (row < 0) return pieces;
    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t modelField = layout ? (*layout)["LeftModel"] : 1u;
    const uint32_t textureField = layout ? (*layout)["LeftModelTexture"] : 3u;
    constexpr const char* kDir = "Item\\ObjectComponents\\Shoulder\\";
    for (int i = 0; i < 2; ++i) {
        std::string name = dbc->getString(static_cast<uint32_t>(row), modelField + static_cast<uint32_t>(i));
        if (name.empty()) {
            pieces.push_back({});
            continue;
        }
        // The file names .mdx; what shipped is the .m2.
        if (const size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
        const std::string texture =
            dbc->getString(static_cast<uint32_t>(row), textureField + static_cast<uint32_t>(i));
        pieces.push_back({.modelPath = kDir + name + ".m2",
                          .texturePath = texture.empty() ? std::string() : kDir + texture + ".blp"});
    }
    return pieces;
}

}  // namespace

void itemAttachmentFiles(pipeline::AssetManager& assets, uint32_t helmDisplayId, uint8_t raceId,
                         uint8_t genderId, uint32_t shoulderDisplayId,
                         std::vector<std::string>& modelPaths, std::vector<std::string>& texturePaths) {
    if (helmDisplayId != 0) {
        const HelmVisual helm = resolveHelmVisual(assets, helmDisplayId, raceId, genderId);
        if (helm.valid()) {
            if (!helm.racialModelPath.empty()) modelPaths.push_back(helm.racialModelPath);
            modelPaths.push_back(helm.baseModelPath);
            if (!helm.texturePath.empty()) texturePaths.push_back(helm.texturePath);
        }
    }
    for (const ShoulderPiece& piece : shoulderPieces(assets, shoulderDisplayId)) {
        if (piece.modelPath.empty()) continue;
        modelPaths.push_back(piece.modelPath);
        if (!piece.texturePath.empty()) texturePaths.push_back(piece.texturePath);
    }
}

void attachShoulders(rendering::CharacterRenderer& renderer, pipeline::AssetManager& assets,
                     uint32_t instanceId, uint32_t displayInfoId,
                     const std::function<uint32_t()>& nextModelId, const AssetPrefetch* prefetch) {
    renderer.detachWeapon(instanceId, attachment::kShoulderRight);
    renderer.detachWeapon(instanceId, attachment::kShoulderLeft);
    const std::vector<ShoulderPiece> pieces = shoulderPieces(assets, displayInfoId);
    for (size_t i = 0; i < pieces.size(); ++i) {
        const ShoulderPiece& piece = pieces[i];
        if (piece.modelPath.empty()) continue;
        const uint32_t point = shoulderAttachmentPoint(static_cast<int>(i));
        // The model carries the texture as its own, so it is shared only by
        // wearers of the same model in the same texture.
        const std::string key = piece.modelPath + '|' + piece.texturePath;
        if (const uint32_t shared = renderer.sharedItemModelId(key)) {
            renderer.attachWeapon(instanceId, point, pipeline::M2Model{}, shared, piece.texturePath);
            continue;
        }
        pipeline::M2Model model;
        if (!itemModel(assets, prefetch, piece.modelPath, model)) continue;
        const uint32_t modelId = nextModelId();
        if (renderer.attachWeapon(instanceId, point, model, modelId, piece.texturePath))
            renderer.rememberSharedItemModel(key, modelId);
    }
}

void attachHelm(rendering::CharacterRenderer& renderer, pipeline::AssetManager& assets,
                uint32_t instanceId, uint32_t displayInfoId, uint8_t raceId, uint8_t genderId,
                const std::function<uint32_t()>& nextModelId, const AssetPrefetch* prefetch) {
    renderer.detachWeapon(instanceId, kAttachHelm);
    if (displayInfoId == 0) return;
    const HelmVisual helm = resolveHelmVisual(assets, displayInfoId, raceId, genderId);
    if (!helm.valid()) return;
    // Shared between wearers as the shoulders are (see attachShoulders): by
    // the paths the race's cut is chosen from and the texture it is worn in.
    const std::string key = helm.racialModelPath + '|' + helm.baseModelPath + '|' + helm.texturePath;
    if (const uint32_t shared = renderer.sharedItemModelId(key)) {
        renderer.attachWeapon(instanceId, kAttachHelm, pipeline::M2Model{}, shared, helm.texturePath);
        return;
    }
    pipeline::M2Model model;
    if (helm.racialModelPath.empty() || !itemModel(assets, prefetch, helm.racialModelPath, model)) {
        model = {};
        if (!itemModel(assets, prefetch, helm.baseModelPath, model)) return;
    }
    const uint32_t modelId = nextModelId();
    if (renderer.attachWeapon(instanceId, kAttachHelm, model, modelId, helm.texturePath))
        renderer.rememberSharedItemModel(key, modelId);
}

}  // namespace core
}  // namespace wowee
