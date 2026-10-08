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
                                                            bool useFemaleModel,
                                                            int facialHairId) {
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
        who.facialHairId = facialHairId;

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
        result.faceLayers = pipeline::faceRegionLayers(sections);
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
    // The head's layers - face, facial hair, scalp - on its two regions.
    for (const auto& fl : texInfo.faceLayers) {
        charRenderer->setFaceRegionLayer(fl.path, fl.lower);
        underwearPaths_.push_back(fl.path);
    }
    for (const auto& up : texInfo.underwearPaths) underwearPaths_.push_back(up);

    // Composite body skin + face + underwear overlays
    {
        std::vector<std::string> layers;
        layers.push_back(texInfo.bodySkinPath);
        for (const auto& fl : texInfo.faceLayers) layers.push_back(fl.path);
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
void AppearanceComposer::setItemOverride(int equipSlot, uint32_t displayId, uint8_t inventoryType) {
    if (displayId == 0) itemOverrides_.erase(equipSlot);
    else itemOverrides_[equipSlot] = {displayId, inventoryType};
}

std::optional<uint32_t> AppearanceComposer::itemOverrideDisplay(int equipSlot) const {
    auto it = itemOverrides_.find(equipSlot);
    if (it == itemOverrides_.end()) return std::nullopt;
    return it->second.first;
}

game::Inventory AppearanceComposer::dressedInventory(const game::Inventory& inventory) const {
    game::Inventory dressed = inventory;
    for (const auto& [equipSlot, worn] : itemOverrides_) {
        const auto slot = static_cast<game::EquipSlot>(equipSlot);
        game::ItemDef item = inventory.getEquipSlot(slot).item;
        // Shown, whether or not anything is equipped there: only the
        // display and inventory type are read to draw it.
        if (item.itemId == 0) item.itemId = 1;
        item.displayInfoId = worn.first;
        item.inventoryType = worn.second;
        dressed.setEquipSlot(slot, item);
    }
    return dressed;
}

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
    // A spell's worn helm (CharProc 17) over the equipped one.
    uint32_t displayId = itemOverrideDisplay(static_cast<int>(game::EquipSlot::HEAD)).value_or(0);
    if (displayId == 0) {
        if (headSlot.empty()) return;
        const auto* info = gameHandler_->getItemInfo(headSlot.item.itemId);
        displayId = info && info->valid ? info->displayInfoId : headSlot.item.displayInfoId;
    }
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

void AppearanceComposer::loadEquippedWeapons() {
    attachEquippedWeapons();
}

UnitWeaponItems AppearanceComposer::playerWeaponItems(std::array<UnitWeaponItem, 3>& storage) const {
    UnitWeaponItems present{};
    if (!gameHandler_) return present;
    constexpr game::EquipSlot kSlots[3] = {game::EquipSlot::MAIN_HAND, game::EquipSlot::OFF_HAND,
                                           game::EquipSlot::RANGED};
    const auto& inventory = gameHandler_->getInventory();
    for (size_t i = 0; i < 3; ++i) {
        const auto& equipSlot = inventory.getEquipSlot(kSlots[i]);
        storage[i] = {};
        if (equipSlot.empty()) continue;
        const auto* info = gameHandler_->getItemInfo(equipSlot.item.itemId);
        const bool known = info && info->valid;
        storage[i] = {.sheath = known ? info->sheath : 0,
                      .inventoryType = static_cast<uint8_t>(equipSlot.item.inventoryType),
                      .itemClass = known ? info->itemClass : 0,
                      .subClass = known ? info->subClass : 0};
        present[i] = &storage[i];
    }
    return present;
}

void AppearanceComposer::attachEquippedWeapons() {
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
        uint32_t shoulderDisplay = itemOverrideDisplay(static_cast<int>(game::EquipSlot::SHOULDERS)).value_or(0);
        if (shoulderDisplay == 0 && !shoulders.empty()) {
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
    const UnitWeaponItems present = playerWeaponItems(items);
    const DressedKey key = currentDressKey();
    // The animations' ranged stance and shot follow the ranged state.
    if (renderer_->getAnimationController()) {
        renderer_->getAnimationController()->setRangedWeaponActive(
            static_cast<SheathState>(key.state) == SheathState::Ranged);
    }
    const UnitWeaponDress dress{
        .state = static_cast<SheathState>(key.state),
        .rangedJustPutAway = rangedJustPutAway_,
        .isPlayer = true,
        .unitFlags = key.unitFlags,
        .unitFlags2 = key.unitFlags2,
        .offHandFollowsAnimation = key.offHandFollowsAnimation,
        .reachShown = reach_ ? std::optional<WeaponsShown>(reach_->shown) : std::nullopt};
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
    key.state = static_cast<uint8_t>(sheath_);
    if (!gameHandler_) return key;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (entity) {
        const uint16_t flags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
        const uint16_t flags2 = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2);
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

std::optional<SheathState> AppearanceComposer::fieldSheathState() const {
    if (!gameHandler_) return std::nullopt;
    const uint16_t bytes2 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2);
    if (bytes2 == 0xFFFF) return std::nullopt;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (!entity) return std::nullopt;
    return static_cast<SheathState>(entity->getField(bytes2) & 0xFFu);
}

uint64_t AppearanceComposer::playerChannelObject() const {
    if (!gameHandler_) return 0;
    const uint16_t field = game::fieldIndex(game::UF::UNIT_FIELD_CHANNEL_OBJECT);
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (!entity || field == 0xFFFF) return 0;
    return static_cast<uint64_t>(entity->getField(field)) |
           (static_cast<uint64_t>(entity->getField(static_cast<uint16_t>(field + 1))) << 32);
}

bool AppearanceComposer::classMayDrawRanged() const {
    if (!gameHandler_ || !entitySpawner_) return false;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (!entity) return false;
    const uint16_t bytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
    const uint32_t classId = bytes0 != 0xFFFF ? (entity->getField(bytes0) >> 8) & 0xFFu : 0;
    return entitySpawner_->classMayDrawRanged(classId);
}

void AppearanceComposer::setSheathState(SheathState state, bool fromServer, bool animated) {
    // 0x00736d30, for the active player: a player, never a creature.
    SheathSetInput in{.current = sheath_,
                      .isPlayer = true,
                      .classMayDrawRanged = state == SheathState::Ranged && classMayDrawRanged(),
                      .fromServer = fromServer,
                      .hasModel = sheathInstanceId_ != 0};
    std::array<UnitWeaponItem, 3> storage{};
    const UnitWeaponItems items = playerWeaponItems(storage);
    if (entitySpawner_ && sheathInstanceId_ != 0) {
        in.offHandFollowsAnimation =
            offHandFollowsAnimation(entitySpawner_->animationBehavior(sheathInstanceId_), items[0] != nullptr);
    }
    const auto next = sheathStateChange(state, items, in);
    if (!next) return;
    const SheathState from = sheath_;
    sheath_ = *next;
    if (!fromServer && gameHandler_) gameHandler_->requestSheathState(static_cast<uint8_t>(*next));
    if (!animated) {
        // An immediate change stops the shoulders' reaches (0x00832840 on
        // key bones 3 and 2) and dresses the weapons at once (0x00731f40).
        stopSheathReach();
        return;
    }
    // 0x00736b60: the arms reach; the weapons move with them.
    ReachPlay play;
    reach_ = beginSheathReach(from, *next, items, play);
    playReach(play, items);
    loadEquippedWeapons();
}

void AppearanceComposer::playReach(const ReachPlay& play, const UnitWeaponItems& items) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const uint32_t instanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    for (int hand = 0; hand < 2; ++hand) {
        if (!play.play[hand] || !reach_) continue;
        // Hand 0 is the right arm, the renderer's arm 1.
        const bool playing = charRenderer && instanceId != 0 &&
            charRenderer->playArmAnimation(instanceId, 1 - hand, play.animId[hand],
                                           hand == 0 ? kEventSheathRight : kEventSheathLeft);
        if (!playing) reachHandEvent(hand, true, items);
    }
}

void AppearanceComposer::reachHandEvent(int hand, bool ended, const UnitWeaponItems& items) {
    if (!reach_) return;
    sheathReachSwap(*reach_, hand, items);
    if (ended) playReach(sheathReachEnded(*reach_, hand, items), items);
}

void AppearanceComposer::updateSheathReach() {
    if (!reach_) return;
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const uint32_t instanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    std::array<UnitWeaponItem, 3> storage{};
    const UnitWeaponItems items = playerWeaponItems(storage);
    const WeaponsShown before = reach_->shown;
    if (charRenderer && instanceId != 0) {
        using CR = rendering::CharacterRenderer;
        const uint8_t events = charRenderer->takeArmAnimationEvents(instanceId);
        // The right arm is hand 0.
        if (events & CR::kArmEventRight) reachHandEvent(0, false, items);
        if (events & CR::kArmEventLeft) reachHandEvent(1, false, items);
        if (events & CR::kArmEndRight) reachHandEvent(0, true, items);
        if (events & CR::kArmEndLeft) reachHandEvent(1, true, items);
    }
    const bool armsIdle = !charRenderer || instanceId == 0 ||
                          (!charRenderer->armAnimation(instanceId, 0) && !charRenderer->armAnimation(instanceId, 1));
    if (armsIdle) {
        // Done: the weapons stay where the reach left them - a ranged
        // weapon put away until the next dressing.
        rangedJustPutAway_ = reach_->shown.ranged == RangedShown::Away && sheath_ == SheathState::Melee;
        reach_.reset();
        loadEquippedWeapons();
    } else if (!(reach_->shown == before)) {
        loadEquippedWeapons();
    }
}

void AppearanceComposer::stopSheathReach() {
    if (!reach_) return;
    reach_.reset();
    if (renderer_ && renderer_->getCharacterRenderer() && renderer_->getCharacterInstanceId() != 0) {
        renderer_->getCharacterRenderer()->stopArmAnimations(renderer_->getCharacterInstanceId());
    }
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
    const bool rangedDrawable =
        !inventory.getEquipSlot(game::EquipSlot::RANGED).empty() && classMayDrawRanged();
    // Not without both reaches, Sheath and HipSheath, and both shoulders
    // (key bones 3 and 2), nor while either shoulder reaches.
    if (auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        const uint32_t instanceId = renderer_->getCharacterInstanceId();
        if (instanceId == 0) return false;
        if (!cr->hasAnimation(instanceId, kAnimHipSheath) || !cr->hasAnimation(instanceId, kAnimSheath)) return false;
        if (!cr->hasKeyBone(instanceId, 3) || !cr->hasKeyBone(instanceId, 2)) return false;
        for (int arm = 0; arm < 2; ++arm) {
            const auto playing = cr->armAnimation(instanceId, arm);
            if (playing && (*playing == kAnimSheath || *playing == kAnimHipSheath)) return false;
        }
    }
    const SheathState current = sheath_;
    const SheathState next = toggledSheathState(current, hasMainOrOff, rangedDrawable);
    if (next == current) return false;
    setSheathState(next, false, true);
    return sheath_ != current;
}

void AppearanceComposer::onSpellCastBegin(uint32_t spellId) {
    if (!gameHandler_ || !entitySpawner_ || sheathInstanceId_ == 0) return;
    uint32_t displayId = 0;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    const uint16_t displayField = game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID);
    if (entity && displayField != 0xFFFF) displayId = entity->getField(displayField);
    if (const auto state = spellSheathState(entitySpawner_->spellSheathInput(spellId, displayId))) {
        setSheathState(*state);
    }
}

TextEmoteVerdict AppearanceComposer::onTextEmote(uint32_t textEmoteId) {
    // Without the player or the tables there is nothing to judge it by.
    if (!gameHandler_ || !assetManager_) return TextEmoteVerdict::Send;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (!entity) return TextEmoteVerdict::Send;
    TextEmoteSheathInput in;
    // EmotesText +8 (EmoteRef) names the Emotes row: its EmoteFlags (+0xc)
    // and EmoteSpecProc (+0x10).
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* textLayout = layouts ? layouts->getLayout("EmotesText") : nullptr;
    const auto* emoteLayout = layouts ? layouts->getLayout("Emotes") : nullptr;
    auto texts = assetManager_->loadDBCOptional("EmotesText.dbc");
    auto emotes = assetManager_->loadDBCOptional("Emotes.dbc");
    if (!texts || !emotes) return TextEmoteVerdict::Send;
    const int32_t textRow = texts->findRecordById(textEmoteId);
    const uint32_t refField = textLayout ? textLayout->tryField("EmoteRef") : 2;
    if (textRow >= 0 && refField < texts->getFieldCount()) {
        const int32_t emoteRow =
            emotes->findRecordById(texts->getUInt32(static_cast<uint32_t>(textRow), refField));
        if (emoteRow >= 0) {
            in.emoteKnown = true;
            const uint32_t flagsField = emoteLayout ? emoteLayout->tryField("EmoteFlags") : 3;
            const uint32_t procField = emoteLayout ? emoteLayout->tryField("EmoteSpecProc") : 4;
            const auto r = static_cast<uint32_t>(emoteRow);
            if (flagsField < emotes->getFieldCount()) in.emoteFlags = emotes->getUInt32(r, flagsField);
            if (procField < emotes->getFieldCount()) in.specProc = emotes->getUInt32(r, procField);
        }
    }
    in.standState = gameHandler_->getStandState();
    in.moveFlags = gameHandler_->getMovementInfo().flags;
    // A flight path is the server's flying spline on the player (0x004f5260).
    in.onFlyingSpline = gameHandler_->isOnTaxiFlight();
    const uint16_t flags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    if (flags != 0xFFFF) in.unitFlags = entity->getField(flags);
    if (const uint16_t charmedBy = game::fieldIndex(game::UF::UNIT_FIELD_CHARMEDBY); charmedBy != 0xFFFF) {
        in.charmed = entity->getField(charmedBy) != 0 || entity->getField(static_cast<uint16_t>(charmedBy + 1)) != 0;
    }
    const TextEmoteVerdict verdict = textEmoteVerdict(in);
    if (verdict == TextEmoteVerdict::Send && sheathInstanceId_ != 0) setSheathState(SheathState::Unarmed);
    return verdict;
}

void AppearanceComposer::updateWeaponsFromFields() {
    const uint32_t instanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    const std::optional<SheathState> field = fieldSheathState();
    if (instanceId != sheathInstanceId_) {
        // 0x0073f660: a new model starts in the field's state; held without
        // the field.
        sheathInstanceId_ = instanceId;
        // A new model has no reach going: its shoulders start on the body.
        reach_.reset();
        sheath_ = field.value_or(SheathState::Melee);
        fieldSheathSeen_ = field;
        animationSheathKey_ = {};
        standSeen_ = gameHandler_ ? gameHandler_->getStandState() : 0;
        channelObjectSeen_ = playerChannelObject();
    } else if (field && fieldSheathSeen_ && *field != *fieldSheathSeen_) {
        // 0x00737aa0: the server's change, taken when the player was in the
        // state it changed from.
        if (const auto state = fieldSheathChange(sheath_, *fieldSheathSeen_, *field, true)) {
            setSheathState(*state, true);
        }
        fieldSheathSeen_ = field;
    } else if (field && !fieldSheathSeen_) {
        fieldSheathSeen_ = field;
    }
    if (instanceId == 0) return;

    if (gameHandler_) {
        // 0x0073f060 on a change of the player's stand state: its own
        // (0x006dcb40) or the server's (SMSG_STANDSTATE_UPDATE).
        if (const uint8_t stand = gameHandler_->getStandState(); stand != standSeen_) {
            standSeen_ = stand;
            if (standStateSheathes(sheath_, stand)) setSheathState(SheathState::Unarmed);
        }
        // 0x0073f4f0 -> 0x0073a520 on a change of its channel object: a
        // channel at a fishing bobber draws the pole.
        if (const uint64_t channelObject = playerChannelObject(); channelObject != channelObjectSeen_) {
            channelObjectSeen_ = channelObject;
            uint32_t objectType = 0;
            if (auto object = channelObject ? gameHandler_->getEntityManager().getEntity(channelObject) : nullptr;
                object && object->getType() == game::ObjectType::GAMEOBJECT) {
                const auto* info = gameHandler_->getCachedGameObjectInfo(
                    static_cast<const game::GameObject&>(*object).getEntry());
                const uint16_t goBytes1 = game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1);
                objectType = info ? info->type : (goBytes1 != 0xFFFF ? (object->getField(goBytes1) >> 8) & 0xFFu : 0);
            }
            uint32_t channelSpell = 0;
            const uint16_t spellField = game::fieldIndex(game::UF::UNIT_CHANNEL_SPELL);
            auto self = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
            if (self && spellField != 0xFFFF) channelSpell = self->getField(spellField);
            if (const auto state = channelSheathState(sheath_, objectType, channelSpell)) setSheathState(*state);
        }
    }

    // The sheath key's reach: its arms' events and ends.
    updateSheathReach();

    // 0x00738180 after each change of animation, cast or attack.
    if (gameHandler_ && entitySpawner_) {
        const auto anim = entitySpawner_->animationRecord(instanceId);
        uint32_t castSpellId = gameHandler_->isCasting() || gameHandler_->isChanneling()
                                   ? gameHandler_->getCurrentCastSpellId()
                                   : 0;
        const AnimationSheathKey key{.animId = anim.animId,
                                     .castSpellId = castSpellId,
                                     .attacking = gameHandler_->isAutoAttacking()};
        if (!(key == animationSheathKey_)) {
            animationSheathKey_ = key;
            AnimationSheathInput in{.current = sheath_,
                                    .animId = anim.animId,
                                    .animKnown = anim.known,
                                    .weaponFlags = anim.weaponFlags,
                                    .behavior = anim.behavior,
                                    .casting = castSpellId != 0,
                                    .attacking = key.attacking,
                                    .activePlayer = true,
                                    .field = field.value_or(sheath_)};
            if (castSpellId != 0) {
                const auto attributes = gameHandler_->getSpellAttributes(castSpellId);
                in.castSheathes = attributes && (*attributes & 0x40000u) == 0;
            }
            if (const auto state = animationSheathState(in)) setSheathState(*state);
            if (const auto idle = animationKitIdle(in)) kitIdle_ = *idle;
        }
    }

    const DressedKey key = currentDressKey();
    if (key == dressedKey_) return;
    // 0x00731f40: from ranged to melee the ranged weapon is put away.
    rangedJustPutAway_ = dressedKey_.instanceId == key.instanceId &&
                         dressedKey_.state == static_cast<uint8_t>(SheathState::Ranged) &&
                         key.state == static_cast<uint8_t>(SheathState::Melee);
    loadEquippedWeapons();
}

} // namespace core
} // namespace wowee
