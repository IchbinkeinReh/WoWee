#include "rendering/character_preview_warm.hpp"

#include "core/character_component.hpp"
#include "core/item_attachments.hpp"
#include "core/preview_dressing.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/item_textures.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/glue_scene.hpp"

#include <string>
#include <unordered_set>

namespace wowee {
namespace rendering {

size_t warmCharacterPreviewFiles(pipeline::AssetManager& assets, const PreviewWarmCharacter& who) {
    if (!assets.isInitialized()) return 0;
    std::unordered_set<std::string> seen;
    size_t read = 0;

    auto texture = [&](const std::string& path) {
        if (path.empty() || !seen.insert(path).second) return;
        if (!assets.fileExists(path)) return;
        (void)assets.readFile(path);
        ++read;
    };
    // A model, its skin, every sequence kept outside it (the preview loads
    // them all, CharacterPreview::loadCharacter) and the textures it names.
    auto model = [&](const std::string& path, bool animations) {
        if (path.empty() || !seen.insert(path).second) return;
        if (!assets.fileExists(path)) return;
        pipeline::M2Model m;
        const bool ok = animations ? pipeline::loadM2WithAllAnimations(assets, path, m)
                                   : pipeline::loadM2WithSkin(assets, path, m);
        ++read;
        if (!ok) return;
        for (const auto& t : m.textures) {
            if (t.type == 0) texture(t.filename);
        }
    };

    // The character (CharacterPreview::loadCharacter).
    model(game::getPlayerModelPath(who.race, who.gender, who.useFemaleModel), true);
    const bool female = who.gender == game::Gender::FEMALE ||
                        (who.gender == game::Gender::NONBINARY && who.useFemaleModel);
    if (auto sections = assets.loadDBC("CharSections.dbc")) {
        const auto* layout = pipeline::getActiveDBCLayout()
                                 ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        const auto fields = pipeline::detectCharSectionsFields(sections.get(), layout);
        pipeline::CharacterAppearance look;
        look.raceId = static_cast<uint32_t>(who.race);
        look.sexId = female ? 1u : 0u;
        look.skinId = static_cast<uint8_t>(who.appearanceBytes & 0xFF);
        look.faceId = static_cast<uint8_t>((who.appearanceBytes >> 8) & 0xFF);
        look.hairStyleId = static_cast<uint8_t>((who.appearanceBytes >> 16) & 0xFF);
        look.hairColorId = static_cast<uint8_t>((who.appearanceBytes >> 24) & 0xFF);
        look.facialHairId = who.facialFeatures;
        const auto textures = pipeline::resolveCharacterSections(sections.get(), fields, look);
        texture(textures.bodySkin);
        texture(textures.skinExtra);
        texture(textures.hair);
        for (const auto& layer : pipeline::faceRegionLayers(textures)) texture(layer.path);
        for (const auto& path : textures.underwear) texture(path);
    }

    // The racial backdrop (CharacterPreview::loadRacialBackdrop).
    if (const char* scene = glue_scene::backgroundScene(who.race, who.classId)) {
        model(std::string("Interface\\Glues\\Models\\") + scene + "\\" + scene + ".m2", true);
    }

    // The gear the select screen dresses it in (applyCharacterSelectEquipment).
    std::vector<core::PreviewItem> slots;
    slots.reserve(who.equipment.size());
    for (const auto& it : who.equipment) {
        slots.push_back({.display = it.displayModel, .inventoryType = it.inventoryType, .enchant = it.enchantment});
    }
    const core::PreviewDress dress = core::characterSelectDress(slots, who.classId, who.flags);
    auto displayInfo = assets.loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfo || !displayInfo->isLoaded()) return read;

    for (const auto& w : dress.held) {
        const int32_t rec = displayInfo->findRecordById(w.display);
        if (w.display == 0 || rec < 0) continue;
        const auto art = pipeline::readItemDisplayArt(*displayInfo, static_cast<uint32_t>(rec));
        const std::string dir = w.shield ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";
        if (!art.modelFile.empty()) model(dir + art.modelFile, false);
        if (!art.textureName.empty()) texture(dir + art.textureName + ".blp");
    }

    uint32_t shoulderDisplay = 0;
    for (const auto& it : dress.worn) {
        if (it.display != 0 && it.inventoryType == 3) {
            shoulderDisplay = it.display;
            break;
        }
    }
    std::vector<std::string> attachModels;
    std::vector<std::string> attachTextures;
    core::itemAttachmentFiles(assets, dress.head, static_cast<uint8_t>(who.race), female ? 1 : 0,
                              shoulderDisplay, attachModels, attachTextures);
    for (const auto& path : attachModels) model(path, false);
    for (const auto& path : attachTextures) texture(path);

    std::vector<core::ComponentItem> components;
    uint32_t capeDisplay = 0;
    for (const auto& it : dress.worn) {
        if (it.display == 0) continue;
        components.push_back({core::componentItemForInventoryType(it.inventoryType), it.display});
        if (it.inventoryType == 16) capeDisplay = it.display;
    }
    for (const auto& [region, path] :
         core::characterComponentLayers(assets, *displayInfo, components, female, who.emblem)) {
        (void)region;
        texture(path);
    }
    if (capeDisplay != 0) {
        const int32_t rec = displayInfo->findRecordById(capeDisplay);
        if (rec >= 0) {
            // LeftModelTexture only, as the client (0x004f21e0).
            const std::string name = displayInfo->getString(static_cast<uint32_t>(rec), 3u);
            for (const auto& c : pipeline::capeTextureCandidates(name, female)) texture(c);
        }
    }
    return read;
}

}  // namespace rendering
}  // namespace wowee
