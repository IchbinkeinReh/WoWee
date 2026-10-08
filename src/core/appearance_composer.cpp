#include "core/appearance_composer.hpp"
#include "core/geoset_rules.hpp"
#include "pipeline/item_textures.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "core/character_paths.hpp"
#include "core/helm_visual.hpp"
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/game_handler.hpp"
#include "core/item_attachments.hpp"
#include "core/weapon_attachment.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace core {

namespace {

constexpr uint32_t kAttachRightHand = attachment::kHandRight;

/// The slot a weapon is drawn from, and its item's Sheath and kind, for
/// core::weaponAttachmentPoint (0x004eacd0).
struct DrawnWeapon {
    WeaponSlot slot;
    uint32_t sheath;
    bool shield;
    bool rangedRight;
};

DrawnWeapon drawnWeapon(game::EquipSlot slot, const game::ItemSlot& item,
                        const game::GameHandler* gameHandler) {
    DrawnWeapon w{};
    w.slot = slot == game::EquipSlot::MAIN_HAND ? WeaponSlot::MainHand
           : slot == game::EquipSlot::OFF_HAND  ? WeaponSlot::OffHand
                                                : WeaponSlot::Ranged;
    const auto* info = gameHandler ? gameHandler->getItemInfo(item.item.itemId) : nullptr;
    w.sheath = info && info->valid ? info->sheath : 0;
    w.shield = item.item.inventoryType == game::InvType::SHIELD;
    w.rangedRight = rangedInRightHand(item.item.inventoryType);
    return w;
}

} // namespace

AppearanceComposer::AppearanceComposer(rendering::Renderer* renderer,
                                       pipeline::AssetManager* assetManager,
                                       game::GameHandler* gameHandler,
                                       EntitySpawner* entitySpawner)
    : renderer_(renderer)
    , assetManager_(assetManager)
    , gameHandler_(gameHandler)
    , entitySpawner_(entitySpawner)
{
}

PlayerTextureInfo AppearanceComposer::resolvePlayerTextures(pipeline::M2Model& model,
                                                            game::Race race, game::Gender gender,
                                                            uint32_t appearanceBytes,
                                                            bool useFemaleModel) {
    PlayerTextureInfo result;

    uint32_t targetRaceId = static_cast<uint32_t>(race);
    // The same rule the model path uses: a nonbinary character wears the body
    // they chose, and the skins have to be that body's.
    const bool female = (gender == game::Gender::FEMALE) ||
                        (gender == game::Gender::NONBINARY && useFemaleModel);
    uint32_t targetSexId = female ? 1u : 0u;

    const char* raceFolderName = raceModelFolder(targetRaceId);
    result.bodySkinPath = defaultBodySkinPath(targetRaceId, targetSexId);

    const AppearanceBytes look = unpackAppearanceBytes(appearanceBytes);
    const uint8_t charSkinId = look.skinId;
    const uint8_t charFaceId = look.faceId;
    const uint8_t charHairStyleId = look.hairStyleId;
    const uint8_t charHairColorId = look.hairColorId;
    LOG_INFO("Appearance: skin=", static_cast<int>(charSkinId), " face=", static_cast<int>(charFaceId),
             " hairStyle=", static_cast<int>(charHairStyleId), " hairColor=", static_cast<int>(charHairColorId));

    // CharSections, through the one reader in pipeline/char_sections.hpp.
    //
    // This scan used to be written out here, and again in entity_spawner for
    // NPCs, and again in character_preview for the portrait - three readings of
    // one table that did not agree on what they read. Only this one looked at
    // the skin row's second texture, so ears and eyelashes were unbound on the
    // other two; only this one had a fallback for a face the table does not
    // carry. Every fault in this area had to be fixed two or three times.
    auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc");
    if (charSectionsDbc) {
        const auto* csL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        const auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);

        pipeline::CharacterAppearance who;
        who.raceId = targetRaceId;
        who.sexId = targetSexId;
        who.skinId = charSkinId;
        who.faceId = charFaceId;
        who.hairStyleId = charHairStyleId;
        who.hairColorId = charHairColorId;

        // The underwear rows name art that is not always on disk, and this
        // caller can ask.
        const auto sections = pipeline::resolveCharacterSections(
            charSectionsDbc.get(), csF, who,
            [](const std::string& path, void* ctx) {
                return static_cast<pipeline::AssetManager*>(ctx)->fileExists(path);
            },
            assetManager_);

        if (!sections.bodySkin.empty()) result.bodySkinPath = sections.bodySkin;
        result.skinExtraPath = sections.skinExtra;
        result.faceLowerPath = sections.faceLower;
        result.faceUpperPath = sections.faceUpper;
        if (!sections.hair.empty()) result.hairTexturePath = sections.hair;
        result.underwearPaths = sections.underwear;

        if (!sections.exactFace) {
            LOG_WARNING("No DBC face match for face=", static_cast<int>(charFaceId),
                        " skin=", static_cast<int>(charSkinId),
                        " race=", targetRaceId, " sex=", targetSexId,
                        sections.haveFace ? " - using the nearest face instead"
                                          : " - this character will render with no face");
        }
        if (!sections.haveHair) {
            LOG_WARNING("No DBC hair match for style=", static_cast<int>(charHairStyleId),
                        " color=", static_cast<int>(charHairColorId),
                        " race=", targetRaceId, " sex=", targetSexId);
        }
    } else {
        LOG_WARNING("Failed to load CharSections.dbc, using hardcoded textures");
    }

    // pipeline/char_sections.hpp fills the runtime slots - the same rules the
    // portrait and the NPC path use, in one place.
    {
        pipeline::CharacterSectionTextures resolved;
        resolved.bodySkin  = result.bodySkinPath;
        resolved.skinExtra = result.skinExtraPath;
        resolved.hair      = result.hairTexturePath;
        resolved.underwear = result.underwearPaths;
        pipeline::applyCharacterTextures(model, resolved, raceFolderName);
    }

    // Everything the head detail depends on, in one line, whichever way it went.
    // Skin-coloured eyelashes are what you see when type 8 falls back to the
    // body or pelvis art, and the three things that decide it - whether the
    // model asks for type 8, whether CharSections offered an extra texture, and
    // what was bound in the end - cannot be told apart from a screenshot.
    {
        bool modelWantsExtra = false;
        std::string bound;
        for (const auto& tex : model.textures) {
            if (tex.type == 8) { modelWantsExtra = true; bound = tex.filename; break; }
        }
        LOG_WARNING("Character head detail: model asks for type 8: ",
                    (modelWantsExtra ? "yes" : "no"),
                    " | CharSections extra: '", result.skinExtraPath,
                    "' | bound: '", bound,
                    "' | body: '", result.bodySkinPath, "'");
    }

    return result;
}

void AppearanceComposer::compositePlayerSkin(uint32_t modelSlotId, const PlayerTextureInfo& texInfo) {
    if (!renderer_) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;

    // Save skin composite state for re-compositing on equipment changes
    // Include face textures so compositeWithRegions can rebuild the full base
    bodySkinPath_ = texInfo.bodySkinPath;
    underwearPaths_.clear();
    if (!texInfo.faceLowerPath.empty()) underwearPaths_.push_back(texInfo.faceLowerPath);
    if (!texInfo.faceUpperPath.empty()) underwearPaths_.push_back(texInfo.faceUpperPath);
    for (const auto& up : texInfo.underwearPaths) underwearPaths_.push_back(up);

    // Composite body skin + face + underwear overlays
    {
        std::vector<std::string> layers;
        layers.push_back(texInfo.bodySkinPath);
        if (!texInfo.faceLowerPath.empty()) layers.push_back(texInfo.faceLowerPath);
        if (!texInfo.faceUpperPath.empty()) layers.push_back(texInfo.faceUpperPath);
        for (const auto& up : texInfo.underwearPaths) {
            layers.push_back(up);
        }
        if (layers.size() > 1) {
            rendering::VkTexture* compositeTex = charRenderer->compositeTextures(layers);
            if (compositeTex != nullptr) {
                // Find type-1 (skin) texture slot and replace with composite
                // We need model texture info - walk slots via charRenderer
                // Use the model slot ID to find the right texture index
                auto* modelData = charRenderer->getModelData(modelSlotId);
                if (modelData) {
                    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                        if (modelData->textures[ti].type == 1) {
                            charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), compositeTex);
                            skinTextureSlotIndex_ = static_cast<uint32_t>(ti);
                            LOG_INFO("Replaced type-1 texture slot ", ti, " with composited body+face+underwear");
                            break;
                        }
                    }
                }
            }
        }
    }

    // Override hair texture on GPU (type-6 slot) after model load
    if (!texInfo.hairTexturePath.empty()) {
        rendering::VkTexture* hairTex = charRenderer->loadTexture(texInfo.hairTexturePath);
        if (hairTex) {
            auto* modelData = charRenderer->getModelData(modelSlotId);
            if (modelData) {
                for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                    if (modelData->textures[ti].type == 6) {
                        charRenderer->setModelTexture(modelSlotId, static_cast<uint32_t>(ti), hairTex);
                        LOG_INFO("Applied DBC hair texture to slot ", ti, ": ", texInfo.hairTexturePath);
                        break;
                    }
                }
            }
        }
    }

    // Find cloak (type-2, Object Skin) texture slot index
    {
        auto* modelData = charRenderer->getModelData(modelSlotId);
        if (modelData) {
            for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                if (modelData->textures[ti].type == 2) {
                    cloakTextureSlotIndex_ = static_cast<uint32_t>(ti);
                    LOG_INFO("Cloak texture slot: ", ti);
                    break;
                }
            }
        }
    }
}

core::CharacterLook AppearanceComposer::playerLook(const game::Character& ch) const {
    core::CharacterLook look;
    look.raceId = static_cast<uint8_t>(ch.race);
    look.genderId = static_cast<uint8_t>(ch.gender);
    look.classId = static_cast<uint8_t>(ch.characterClass);
    // PLAYER_BYTES: skin, face, hair style, hair colour.
    look.skinId = static_cast<uint8_t>(ch.appearanceBytes & 0xFF);
    look.faceId = static_cast<uint8_t>((ch.appearanceBytes >> 8) & 0xFF);
    const auto hairStyle = static_cast<uint8_t>((ch.appearanceBytes >> 16) & 0xFF);
    if (entitySpawner_) {
        const auto& tables = entitySpawner_->getAppearanceGeosetTables();
        look.hairGeoset = tables.hairGeoset(look.raceId, look.genderId, hairStyle);
        if (const auto* f = tables.facialColumns(look.raceId, look.genderId, ch.facialFeatures)) {
            look.facial = *f;
        }
    }
    return look;
}

std::unordered_set<uint16_t> AppearanceComposer::playerGeosets(const core::CharacterLook& look,
                                                               uint32_t instanceId) const {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || !assetManager_) return {};
    std::vector<uint16_t> ids;
    if (const auto* md = charRenderer->getInstanceModelData(instanceId)) {
        for (const auto& batch : md->batches) ids.push_back(batch.submeshId);
    }
    return core::modelGeosetsShown(core::characterLookGeosets(*assetManager_, look), ids);
}

void AppearanceComposer::applyEnchantVisuals(uint32_t charInstanceId, int equipSlotIndex,
                                             uint32_t attachmentId) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || !gameHandler_ || !assetManager_ || !entitySpawner_) return;

    charRenderer->detachWeaponEffects(charInstanceId, attachmentId);

    uint64_t itemGuid = gameHandler_->getEquipSlotGuid(equipSlotIndex);
    if (itemGuid == 0) return;

    // A temporary enchant (sharpening stone, oil) masks the permanent one's visual.
    auto [permEnchantId, tempEnchantId] = gameHandler_->getItemEnchantIds(itemGuid);
    uint32_t enchantId = (tempEnchantId != 0) ? tempEnchantId : permEnchantId;
    if (enchantId == 0) return;

    auto sieDbc     = assetManager_->loadDBC("SpellItemEnchantment.dbc");
    auto visualsDbc = assetManager_->loadDBC("ItemVisuals.dbc");
    auto effectsDbc = assetManager_->loadDBC("ItemVisualEffects.dbc");
    if (!sieDbc || !sieDbc->isLoaded() || !visualsDbc || !visualsDbc->isLoaded() ||
        !effectsDbc || !effectsDbc->isLoaded()) {
        return;
    }

    const auto* sieL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
    auto effectModels = pipeline::resolveEnchantItemVisuals(enchantId, sieDbc.get(),
                                                            visualsDbc.get(), effectsDbc.get(), sieL);

    for (uint32_t visualSlot = 0; visualSlot < effectModels.size(); ++visualSlot) {
        const std::string& modelName = effectModels[visualSlot];
        if (modelName.empty()) continue;

        // DBC stores .mdx paths; the shipped assets are .m2.
        std::string m2Path = modelName;
        size_t dotPos = m2Path.rfind('.');
        m2Path = (dotPos != std::string::npos ? m2Path.substr(0, dotPos) : m2Path) + ".m2";

        pipeline::M2Model effectModel;
        if (!loadWeaponM2(m2Path, effectModel)) {
            LOG_WARNING("Enchant visual: failed to load ", m2Path);
            continue;
        }

        uint32_t effectModelId = entitySpawner_->allocateWeaponModelId();
        if (charRenderer->attachWeaponEffect(charInstanceId, attachmentId, visualSlot,
                                             effectModel, effectModelId)) {
            LOG_INFO("Enchant visual: ", m2Path, " on attachment ", attachmentId,
                     " (enchant ", enchantId, ", visual slot ", visualSlot, ")");
        }
    }
}

bool AppearanceComposer::loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    // pipeline/m2_asset_loader.hpp. Kept as a method because a dozen call sites
    // read better for it.
    return pipeline::loadM2WithSkin(*assetManager_, m2Path, outModel);
}

// Head gear, which only other players used to get. The local character's
// appearance is assembled here while everyone else's goes through
// EntitySpawner, and the head slot was simply missing from this side: no
// helmet model, and hair left showing through where one should be.
void AppearanceComposer::loadEquippedHelm(game::Inventory& inventory) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const uint32_t charInstanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    if (!charRenderer || charInstanceId == 0 || !assetManager_ || !gameHandler_) return;

    // Only the helm point. Detaching 0 as well would drop the shield.
    charRenderer->detachWeapon(charInstanceId, kAttachHelm);

    // Hiding the helm is a display choice, not an unequip: the item stays on,
    // the model comes off, and the hair comes back.
    if (!gameHandler_->isHelmVisible()) return;

    const auto& headSlot = inventory.getEquipSlot(game::EquipSlot::HEAD);
    if (headSlot.empty()) return;
    const auto* info = gameHandler_->getItemInfo(headSlot.item.itemId);
    const uint32_t displayId = info && info->valid ? info->displayInfoId
                                                   : headSlot.item.displayInfoId;
    if (displayId == 0) return;

    uint8_t raceId = 0;
    uint8_t genderId = 0;
    if (const auto* ch = gameHandler_->getActiveCharacter()) {
        raceId = static_cast<uint8_t>(ch->race);
        genderId = static_cast<uint8_t>(ch->gender);
    }

    const core::HelmVisual helm =
        core::resolveHelmVisual(*assetManager_, displayId, raceId, genderId);
    if (!helm.valid()) return;

    pipeline::M2Model helmModel;
    std::string helmPath;
    if (!helm.racialModelPath.empty()) {
        helmPath = helm.racialModelPath;
        if (!loadWeaponM2(helmPath, helmModel)) helmModel = {};
    }
    if (!helmModel.isValid()) {
        helmPath = helm.baseModelPath;
        if (!loadWeaponM2(helmPath, helmModel)) return;
    }

    const uint32_t helmModelId = entitySpawner_ ? entitySpawner_->allocateWeaponModelId() : 0;
    const bool attached = charRenderer->attachWeapon(charInstanceId, kAttachHelm, helmModel,
                                                     helmModelId, helm.texturePath);
    if (attached) {
        LOG_INFO("Equipped helm: ", helmPath, " tex: ", helm.texturePath);
    }
}

rendering::SheathSpot AppearanceComposer::sheathSpot(game::EquipSlot slot) const {
    if (!gameHandler_) return rendering::SheathSpot::NONE;
    const auto& equipped = gameHandler_->getInventory().getEquipSlot(slot);
    if (equipped.empty()) return rendering::SheathSpot::NONE;
    const DrawnWeapon w = drawnWeapon(slot, equipped, gameHandler_);
    switch (weaponAttachmentPoint(w.slot, w.sheath, true, w.shield, w.rangedRight)) {
        case attachment::kHipWeaponLeft:
        case attachment::kHipWeaponRight: return rendering::SheathSpot::HIP;
        case attachment::kSheathMainHand:
        case attachment::kSheathOffHand:
        case attachment::kSheathShield:
        case attachment::kLargeWeaponLeft:
        case attachment::kLargeWeaponRight: return rendering::SheathSpot::BACK;
        default:                            return rendering::SheathSpot::NONE;
    }
}

void AppearanceComposer::loadEquippedWeapons() {
    // Equipment refreshes can arrive during a gather cast. Keep the temporary
    // tool authoritative until the cast-end callback restores real equipment.
    const uint32_t currentInstanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    if (showingMiningPick_ && currentInstanceId == miningPickInstanceId_) return;
    if (showingMiningPick_) {
        showingMiningPick_ = false;
        miningPickInstanceId_ = 0;
    }
    showingRanged_ = false;
    if (renderer_ && renderer_->getAnimationController())
        renderer_->getAnimationController()->setRangedWeaponActive(false);
    attachEquippedWeapons(false);
}

void AppearanceComposer::attachEquippedWeapons(bool rangedDrawn) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized())
        return;
    if (!gameHandler_ || !entitySpawner_) return;

    auto* charRenderer = renderer_->getCharacterRenderer();
    uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    auto& inventory = gameHandler_->getInventory();

    loadEquippedHelm(inventory);
    // The shoulders (0x004ef840).
    {
        const auto& shoulders = inventory.getEquipSlot(game::EquipSlot::SHOULDERS);
        uint32_t shoulderDisplay = 0;
        if (!shoulders.empty()) {
            const auto* info = gameHandler_->getItemInfo(shoulders.item.itemId);
            shoulderDisplay = info && info->valid ? info->displayInfoId : shoulders.item.displayInfoId;
        }
        attachShoulders(*charRenderer, *assetManager_, charInstanceId, shoulderDisplay,
                        [this] { return entitySpawner_->allocateWeaponModelId(); });
    }

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) {
        LOG_WARNING("loadEquippedWeapons: failed to load ItemDisplayInfo.dbc");
        return;
    }

    // Equipment reloads and sheath changes move models between these points;
    // clear every one so no old copy stays behind.
    for (uint32_t point : {attachment::kShield, attachment::kHandRight, attachment::kHandLeft,
                           attachment::kSheathMainHand, attachment::kSheathOffHand,
                           attachment::kSheathShield, attachment::kLargeWeaponLeft,
                           attachment::kLargeWeaponRight, attachment::kHipWeaponLeft,
                           attachment::kHipWeaponRight}) {
        charRenderer->detachWeapon(charInstanceId, point);
    }

    // 0x0072dbc0 for each slot, as for every other unit: the three items,
    // the sheath state and the rest of what it reads.
    constexpr game::EquipSlot kSlots[3] = {game::EquipSlot::MAIN_HAND, game::EquipSlot::OFF_HAND,
                                           game::EquipSlot::RANGED};
    constexpr WeaponSlot kWeaponSlots[3] = {WeaponSlot::MainHand, WeaponSlot::OffHand, WeaponSlot::Ranged};
    std::array<UnitWeaponItem, 3> items{};
    UnitWeaponItems present{};
    for (size_t i = 0; i < 3; ++i) {
        const auto& equipSlot = inventory.getEquipSlot(kSlots[i]);
        if (equipSlot.empty()) continue;
        const auto* info = gameHandler_->getItemInfo(equipSlot.item.itemId);
        const bool known = info && info->valid;
        items[i] = {.sheath = known ? info->sheath : 0,
                    .inventoryType = static_cast<uint8_t>(equipSlot.item.inventoryType),
                    .itemClass = known ? info->itemClass : 0,
                    .subClass = known ? info->subClass : 0};
        present[i] = &items[i];
    }
    const DressedKey key = currentDressKey();
    const UnitWeaponDress dress{
        .state = rangedDrawn ? SheathState::Ranged : static_cast<SheathState>(key.state),
        .rangedJustPutAway = !rangedDrawn && rangedJustPutAway_,
        .isPlayer = true,
        .unitFlags = key.unitFlags,
        .unitFlags2 = key.unitFlags2,
        .offHandFollowsAnimation = key.offHandFollowsAnimation};
    dressedKey_ = key;
    rangedJustPutAway_ = false;

    for (size_t i = 0; i < 3; ++i) {
        const game::EquipSlot slot = kSlots[i];
        const auto& equipSlot = inventory.getEquipSlot(slot);
        if (!present[i] || equipSlot.item.displayInfoId == 0) continue;

        const uint32_t attachmentId = unitWeaponPoint(kWeaponSlots[i], present, dress);
        if (attachmentId == attachment::kNone) continue;

        const int32_t recIdx = displayInfoDbc->findRecordById(equipSlot.item.displayInfoId);
        if (recIdx < 0) continue;
        const auto art = pipeline::readItemDisplayArt(*displayInfoDbc, static_cast<uint32_t>(recIdx));
        if (art.modelFile.empty()) continue;

        // A shield in the off hand is a shield model (0x004eacd0); the rest
        // are weapons - with the shield folder still tried for anything else.
        const bool shield = items[i].inventoryType == 14 && kWeaponSlots[i] == WeaponSlot::OffHand;
        const char* firstDir = shield ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";
        const char* secondDir = shield ? "Item\\ObjectComponents\\Weapon\\" : "Item\\ObjectComponents\\Shield\\";
        std::string m2Path = firstDir + art.modelFile;
        std::string dir = firstDir;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) {
            m2Path = secondDir + art.modelFile;
            dir = secondDir;
            if (!loadWeaponM2(m2Path, weaponModel)) {
                LOG_WARNING("loadEquippedWeapons: failed to load ", art.modelFile);
                continue;
            }
        }
        std::string texturePath;
        if (!art.textureName.empty()) {
            texturePath = dir + art.textureName + ".blp";
            if (!assetManager_->fileExists(texturePath)) {
                texturePath = (dir == firstDir ? secondDir : firstDir) + art.textureName + ".blp";
            }
        }

        const uint32_t weaponModelId = entitySpawner_->allocateWeaponModelId();
        if (charRenderer->attachWeapon(charInstanceId, attachmentId, weaponModel, weaponModelId,
                                       texturePath)) {
            LOG_INFO("Equipped weapon: ", m2Path, " at attachment ", attachmentId);
            applyEnchantVisuals(charInstanceId, static_cast<int>(slot), attachmentId);
        }
    }
}

AppearanceComposer::DressedKey AppearanceComposer::currentDressKey() const {
    DressedKey key;
    key.instanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    // Without a sheath field in the layout the weapons are held.
    key.state = static_cast<uint8_t>(SheathState::Melee);
    if (!gameHandler_) return key;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (entity) {
        const uint16_t bytes2 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2);
        const uint16_t flags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
        const uint16_t flags2 = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2);
        if (bytes2 != 0xFFFF) key.state = static_cast<uint8_t>(entity->getField(bytes2) & 0xFFu);
        if (flags != 0xFFFF) key.unitFlags = entity->getField(flags) & kUnitFlagDisarmed;
        if (flags2 != 0xFFFF) key.unitFlags2 = entity->getField(flags2) & kUnitFlag2DisarmOffhand;
    }
    if (entitySpawner_ && key.instanceId != 0) {
        const bool hasMainHand = !gameHandler_->getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND).empty();
        key.offHandFollowsAnimation =
            offHandFollowsAnimation(entitySpawner_->animationBehavior(key.instanceId), hasMainHand);
    }
    return key;
}

SheathState AppearanceComposer::sheathState() const {
    return static_cast<SheathState>(currentDressKey().state);
}

void AppearanceComposer::requestSheathState(SheathState state) {
    if (!gameHandler_) return;
    const auto current = static_cast<uint8_t>(sheathState());
    const auto wanted = static_cast<uint8_t>(state);
    if (wanted == current || (wanted == requestedState_ && current == requestedFrom_)) return;
    requestedState_ = wanted;
    requestedFrom_ = current;
    gameHandler_->requestSheathState(wanted);
}

bool AppearanceComposer::toggleSheath() {
    if (!gameHandler_) return false;
    // 0x006e23a0: not while channelling (+0x40) or casting, stunned
    // (UNIT_FIELD_FLAGS 0x40000) or dead (health +0x48 below 1).
    if (gameHandler_->isCasting() || gameHandler_->isChanneling()) return false;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (!entity) return false;
    const uint16_t flags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    const uint16_t health = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
    if (flags != 0xFFFF && (entity->getField(flags) & 0x40000u) != 0) return false;
    if (health != 0xFFFF && entity->getField(health) == 0) return false;

    const auto& inventory = gameHandler_->getInventory();
    const bool hasMainOrOff = !inventory.getEquipSlot(game::EquipSlot::MAIN_HAND).empty() ||
                              !inventory.getEquipSlot(game::EquipSlot::OFF_HAND).empty();
    // The ranged weapon counts when the class's ChrClasses row (+0x24, the
    // Flags column) does not have 8.
    bool rangedDrawable = !inventory.getEquipSlot(game::EquipSlot::RANGED).empty();
    if (rangedDrawable) {
        const uint16_t bytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
        const uint32_t classId = bytes0 != 0xFFFF ? (entity->getField(bytes0) >> 8) & 0xFFu : 0;
        auto classes = assetManager_ ? assetManager_->loadDBCOptional("ChrClasses.dbc") : nullptr;
        const int32_t row = classes ? classes->findRecordById(classId) : -1;
        if (row < 0) {
            rangedDrawable = false;
        } else if (classes->getFieldCount() == 60) {
            // WotLK's layout: Flags is column 57.
            rangedDrawable = (classes->getUInt32(static_cast<uint32_t>(row), 57) & 8u) == 0;
        }
    }
    const SheathState current = sheathState();
    const SheathState next = toggledSheathState(current, hasMainOrOff, rangedDrawable);
    if (next == current) return false;
    requestedState_ = static_cast<uint8_t>(next);
    requestedFrom_ = static_cast<uint8_t>(current);
    gameHandler_->requestSheathState(requestedState_);
    return true;
}

void AppearanceComposer::updateWeaponsFromFields() {
    // A tool or the ranged swap in the hands keeps them until it ends,
    // which dresses the weapons again.
    if (showingMiningPick_ || showingFishingPole_ || showingRanged_) return;
    const DressedKey key = currentDressKey();
    if (key.instanceId == 0 || key == dressedKey_) return;
    // 0x00731f40: from ranged to melee the ranged weapon is put away.
    rangedJustPutAway_ = dressedKey_.instanceId == key.instanceId &&
                         dressedKey_.state == static_cast<uint8_t>(SheathState::Ranged) &&
                         key.state == static_cast<uint8_t>(SheathState::Melee);
    loadEquippedWeapons();
}

void AppearanceComposer::showMiningPick(bool show) {
    if (show == showingMiningPick_) return;

    if (!show) {
        showingMiningPick_ = false;
        miningPickInstanceId_ = 0;
        loadEquippedWeapons();
        return;
    }

    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ ||
        !assetManager_->isInitialized() || !entitySpawner_) {
        return;
    }

    auto* charRenderer = renderer_->getCharacterRenderer();
    const uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    // Item 2901 (Mining Pick) resolves to ItemDisplayInfo 6568 in the WotLK DBC.
    constexpr uint32_t kMiningPickDisplayId = 6568;
    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;

    const int32_t recIdx = displayInfoDbc->findRecordById(kMiningPickDisplayId);
    if (recIdx < 0) {
        LOG_WARNING("showMiningPick: displayInfoId ", kMiningPickDisplayId,
                    " not found in DBC");
        return;
    }

    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    std::string modelName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
    std::string textureName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
    if (modelName.empty()) return;

    const size_t dotPos = modelName.rfind('.');
    std::string modelFile = dotPos == std::string::npos
        ? modelName + ".m2" : modelName.substr(0, dotPos) + ".m2";
    const std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;

    pipeline::M2Model pickModel;
    if (!loadWeaponM2(m2Path, pickModel)) {
        LOG_WARNING("showMiningPick: failed to load ", m2Path);
        return;
    }

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
    }

    charRenderer->detachWeapon(charInstanceId, kAttachRightHand);
    const uint32_t modelId = entitySpawner_->allocateWeaponModelId();
    if (charRenderer->attachWeapon(charInstanceId, kAttachRightHand, pickModel,
                                   modelId, texturePath)) {
        showingMiningPick_ = true;
        miningPickInstanceId_ = charInstanceId;
        showingRanged_ = false;
        if (renderer_->getAnimationController())
            renderer_->getAnimationController()->setRangedWeaponActive(false);
        LOG_INFO("Mining pick attached at right hand: ", m2Path);
    } else {
        // Do not leave the player empty-handed if the temporary model failed.
        loadEquippedWeapons();
    }
}

void AppearanceComposer::showFishingPole(bool show) {
    if (show == showingFishingPole_) return;

    if (!show) {
        showingFishingPole_ = false;
        loadEquippedWeapons();
        return;
    }
    if (!renderer_ || !renderer_->getCharacterRenderer() || !gameHandler_ ||
        !assetManager_ || !assetManager_->isInitialized() || !entitySpawner_) {
        return;
    }

    const auto isFishingPole = [this](const game::ItemSlot& slot) {
        if (slot.empty()) return false;
        if (slot.item.subclassName == "Fishing Pole") return true;
        const auto* info = gameHandler_->getItemInfo(slot.item.itemId);
        return info && info->valid && info->itemClass == 2 && info->subClass == 20;
    };

    const game::ItemSlot* pole = nullptr;
    const auto& inventory = gameHandler_->getInventory();
    const auto& mainHand = inventory.getEquipSlot(game::EquipSlot::MAIN_HAND);
    if (isFishingPole(mainHand)) pole = &mainHand;
    for (int i = 0; !pole && i < inventory.getBackpackSize(); ++i) {
        const auto& slot = inventory.getBackpackSlot(i);
        if (isFishingPole(slot)) pole = &slot;
    }
    for (int bag = 0; !pole && bag < game::Inventory::NUM_BAG_SLOTS; ++bag) {
        for (int slotIndex = 0; !pole && slotIndex < inventory.getBagSize(bag); ++slotIndex) {
            const auto& slot = inventory.getBagSlot(bag, slotIndex);
            if (isFishingPole(slot)) pole = &slot;
        }
    }
    if (!pole || pole->item.displayInfoId == 0) {
        LOG_WARNING("showFishingPole: no fishing pole with display data found in inventory");
        return;
    }

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const int32_t recIdx = displayInfoDbc->findRecordById(pole->item.displayInfoId);
    if (recIdx < 0) return;

    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    std::string modelName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
    std::string textureName = displayInfoDbc->getString(
        static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
    if (modelName.empty()) return;

    const size_t dotPos = modelName.rfind('.');
    const std::string modelFile = dotPos == std::string::npos
        ? modelName + ".m2" : modelName.substr(0, dotPos) + ".m2";
    std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
    pipeline::M2Model poleModel;
    if (!loadWeaponM2(m2Path, poleModel)) return;

    std::string texturePath;
    if (!textureName.empty()) {
        texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
    }

    auto* charRenderer = renderer_->getCharacterRenderer();
    const uint32_t charInstanceId = renderer_->getCharacterInstanceId();
    if (charInstanceId == 0) return;
    charRenderer->detachWeapon(charInstanceId, kAttachRightHand);
    const uint32_t modelId = entitySpawner_->allocateWeaponModelId();
    if (charRenderer->attachWeapon(charInstanceId, kAttachRightHand, poleModel,
                                   modelId, texturePath)) {
        showingFishingPole_ = true;
        showingRanged_ = false;
        if (renderer_->getAnimationController())
            renderer_->getAnimationController()->setRangedWeaponActive(false);
        LOG_INFO("Fishing pole attached at right hand: ", m2Path);
    } else {
        loadEquippedWeapons();
    }
}

void AppearanceComposer::showRangedWeapon(bool show) {
    if (show == showingRanged_) return;
    if (!show) {
        // Back to the melee weapons as the sheath state has them.
        loadEquippedWeapons();
        return;
    }
    if (!gameHandler_) return;
    const auto& rangedSlot = gameHandler_->getInventory().getEquipSlot(game::EquipSlot::RANGED);
    if (rangedSlot.empty() || rangedSlot.item.displayInfoId == 0) return;
    // The ranged sheath state: the ranged weapon drawn, the others put away.
    showingRanged_ = true;
    if (renderer_ && renderer_->getAnimationController())
        renderer_->getAnimationController()->setRangedWeaponActive(true);
    attachEquippedWeapons(true);
}

} // namespace core
} // namespace wowee
