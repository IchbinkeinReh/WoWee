#include "core/item_attachments.hpp"

#include "core/weapon_attachment.hpp"
#include "core/helm_visual.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/character_renderer.hpp"

#include <string>

namespace wowee {
namespace core {

void attachShoulders(rendering::CharacterRenderer& renderer, pipeline::AssetManager& assets,
                     uint32_t instanceId, uint32_t displayInfoId,
                     const std::function<uint32_t()>& nextModelId) {
    renderer.detachWeapon(instanceId, attachment::kShoulderRight);
    renderer.detachWeapon(instanceId, attachment::kShoulderLeft);
    if (displayInfoId == 0) return;
    auto dbc = assets.loadDBC("ItemDisplayInfo.dbc");
    if (!dbc || !dbc->isLoaded()) return;
    const int32_t row = dbc->findRecordById(displayInfoId);
    if (row < 0) return;
    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t modelField = layout ? (*layout)["LeftModel"] : 1u;
    const uint32_t textureField = layout ? (*layout)["LeftModelTexture"] : 3u;
    constexpr const char* kDir = "Item\\ObjectComponents\\Shoulder\\";
    for (int i = 0; i < 2; ++i) {
        std::string name = dbc->getString(static_cast<uint32_t>(row), modelField + static_cast<uint32_t>(i));
        if (name.empty()) continue;
        // The file names .mdx; what shipped is the .m2.
        if (const size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
        pipeline::M2Model model;
        if (!pipeline::loadM2WithSkin(assets, kDir + name + ".m2", model) || !model.isValid()) continue;
        const std::string texture =
            dbc->getString(static_cast<uint32_t>(row), textureField + static_cast<uint32_t>(i));
        const std::string texturePath = texture.empty() ? std::string() : kDir + texture + ".blp";
        renderer.attachWeapon(instanceId, shoulderAttachmentPoint(i), model, nextModelId(), texturePath);
    }
}

void attachHelm(rendering::CharacterRenderer& renderer, pipeline::AssetManager& assets,
                uint32_t instanceId, uint32_t displayInfoId, uint8_t raceId, uint8_t genderId,
                const std::function<uint32_t()>& nextModelId) {
    renderer.detachWeapon(instanceId, kAttachHelm);
    if (displayInfoId == 0) return;
    const HelmVisual helm = resolveHelmVisual(assets, displayInfoId, raceId, genderId);
    if (!helm.valid()) return;
    pipeline::M2Model model;
    if (helm.racialModelPath.empty() || !pipeline::loadM2WithSkin(assets, helm.racialModelPath, model) ||
        !model.isValid()) {
        model = {};
        if (!pipeline::loadM2WithSkin(assets, helm.baseModelPath, model) || !model.isValid()) return;
    }
    renderer.attachWeapon(instanceId, kAttachHelm, model, nextModelId(), helm.texturePath);
}

}  // namespace core
}  // namespace wowee
