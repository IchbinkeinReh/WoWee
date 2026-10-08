#include "core/entity_spawner.hpp"
#include "core/character_component.hpp"
#include "core/helm_visual.hpp"
#include "core/geoset_rules.hpp"
#include "core/character_geosets.hpp"
#include "core/item_attachments.hpp"
#include "core/character_paths.hpp"
#include "pipeline/char_sections.hpp"

// M2 attachment 11 is the helm. 0 is the shield mount, which is where head gear
// was going: it attached, reported success, and hung off the forearm.
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "audio/npc_voice_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/item_textures.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/transport_manager.hpp"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace core {

namespace {
// The bare geoset ids, the group arithmetic and the appearance key all live in
// core/geoset_rules.hpp. This file used to carry its own copy of the constants,
// and the copy went stale: it named 2002 as "the" bare feet and never learned
// about 2001, which is how an HD model that spells its feet the other way lost
// them here while keeping them in the portrait.

// The head of a character's bare geoset set: the body, the one scalp it wears,
// and its facial hair.
//
// Two places build a player's geosets - one for a player seen across the world,
// one for the equipped composition - and they had already drifted: only one of
// them knew about the second bare-feet id, and only one of them treated a zero
// facial variant as "none" rather than as geoset x00.
} // namespace

core::CharacterLook EntitySpawner::playerLook(uint64_t guid, uint8_t raceId, uint8_t genderId,
                                             uint32_t appearanceBytes, uint8_t facialFeatures) const {
    core::CharacterLook look;
    look.raceId = raceId;
    look.genderId = genderId;
    look.classId = gameHandler_ ? gameHandler_->lookupPlayerClass(guid) : 0;
    // PLAYER_BYTES: skin, face, hair style, hair colour.
    look.skinId = static_cast<uint8_t>(appearanceBytes & 0xFF);
    look.faceId = static_cast<uint8_t>((appearanceBytes >> 8) & 0xFF);
    const auto hairStyle = static_cast<uint8_t>((appearanceBytes >> 16) & 0xFF);
    look.hairGeoset = appearanceTables_.hairGeoset(raceId, genderId, hairStyle);
    if (const auto* f = appearanceTables_.facialColumns(raceId, genderId, facialFeatures)) look.facial = *f;
    return look;
}

std::vector<uint16_t> EntitySpawner::modelSubmeshIds(uint32_t modelId) const {
    std::vector<uint16_t> ids;
    const auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (const auto* md = cr ? cr->getModelData(modelId) : nullptr) {
        for (const auto& batch : md->batches) ids.push_back(batch.submeshId);
    }
    return ids;
}

uint32_t EntitySpawner::readyPlayerModelId(uint32_t cacheKey) {
    auto it = playerModelCache_.find(cacheKey);
    if (it == playerModelCache_.end()) return 0;
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (charRenderer && charRenderer->getModelData(it->second)) return it->second;
    LOG_WARNING("Cached player model missing after world reload, reloading modelId=",
                it->second, " race=", cacheKey >> 8, " gender=", cacheKey & 0xFF);
    playerTextureSlotsByModelId_.erase(it->second);
    playerModelCache_.erase(it);
    return 0;
}

void EntitySpawner::spawnOnlinePlayer(uint64_t guid,
                                    uint8_t raceId,
                                    uint8_t genderId,
                                    uint32_t appearanceBytes,
                                    uint8_t facialFeatures,
                                    float x, float y, float z, float orientation) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;
    if (playerInstances_.count(guid)) return;

    // Skip local player - already spawned as the main character
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        uint64_t activeGuid = gameHandler_->getActiveCharacterGuid();
        if ((localGuid != 0 && guid == localGuid) ||
            (activeGuid != 0 && guid == activeGuid) ||
            (spawnedPlayerGuid_ != 0 && guid == spawnedPlayerGuid_)) {
            return;
        }
    }
    auto* charRenderer = renderer_->getCharacterRenderer();

    // The model is loaded off the main thread by processPlayerSpawnQueue, every
    // external sequence with it, and this only ever draws one that is ready.
    const uint32_t modelId = readyPlayerModelId(playerModelCacheKey(raceId, genderId));
    if (modelId == 0) {
        LOG_WARNING("spawnOnlinePlayer: no model loaded for guid 0x", std::hex, guid, std::dec,
                    " race=", static_cast<int>(raceId), " gender=", static_cast<int>(genderId));
        return;
    }

    // Determine texture slots once per model
    {
        auto [slotIt, inserted] = playerTextureSlotsByModelId_.try_emplace(modelId);
        if (inserted) {
            PlayerTextureSlots slots;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    uint32_t t = md->textures[ti].type;
                    if (t == 1 && slots.skin < 0) slots.skin = static_cast<int>(ti);
                    else if (t == 6 && slots.hair < 0) slots.hair = static_cast<int>(ti);
                    else if (t == 8 && slots.skinExtra < 0) slots.skinExtra = static_cast<int>(ti);
                }
            }
            slotIt->second = slots;
        }
    }

    // Create instance at server position
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    float renderYaw = orientation + glm::radians(90.0f);
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos, glm::vec3(0.0f, 0.0f, renderYaw), 1.0f);
    if (instanceId == 0) return;

    // The character's textures, through the one reader in
    // pipeline/char_sections.hpp - the same scan the local player, the NPCs and
    // the portrait use. This path used to carry a fourth copy of it, and the
    // copy did not read the skin row's second texture, which is the head detail
    // an HD model draws its ears and eyelashes from. Every other player in the
    // world had skin-coloured eyelashes for exactly that reason.
    const std::string defaultSkin = defaultBodySkinPath(raceId, genderId);
    const std::string pelvisPath = defaultPelvisPath(raceId, genderId);
    const AppearanceBytes look = unpackAppearanceBytes(appearanceBytes);

    std::string bodySkinPath = defaultSkin;
    std::string skinExtraPath, hairTexturePath, faceLowerPath, faceUpperPath;
    std::vector<std::string> underwearPaths;

    if (auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc");
        charSectionsDbc && charSectionsDbc->isLoaded()) {
        const auto* csL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        const auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);

        pipeline::CharacterAppearance who;
        who.raceId = raceId;
        who.sexId = genderId;
        who.skinId = look.skinId;
        who.faceId = look.faceId;
        who.hairStyleId = look.hairStyleId;
        who.hairColorId = look.hairColorId;

        // The underwear rows name art that was never shipped for some skin
        // colours - Draenei 10 to 16 among them - and this caller can check.
        const auto sections = pipeline::resolveCharacterSections(
            charSectionsDbc.get(), csF, who,
            [](const std::string& path, void* ctx) {
                return static_cast<pipeline::AssetManager*>(ctx)->fileExists(path);
            },
            assetManager_);

        if (!sections.bodySkin.empty()) bodySkinPath = sections.bodySkin;
        skinExtraPath = sections.skinExtra;
        faceLowerPath = sections.faceLower;
        faceUpperPath = sections.faceUpper;
        hairTexturePath = sections.hair;
        underwearPaths = sections.underwear;

        if (!sections.exactFace) {
            LOG_WARNING("spawnOnlinePlayer: no DBC face match for face=",
                        static_cast<int>(look.faceId), " skin=", static_cast<int>(look.skinId),
                        // Cast, because both are uint8_t and the stream writes
                        // one as a character: race 9 came out as a tab and sex
                        // 0 as a NUL, which is unreadable and puts a NUL in the
                        // log file - grep then takes the whole log for binary
                        // and prints nothing at all for any pattern in it.
                        " race=", static_cast<int>(raceId),
                        " sex=", static_cast<int>(genderId),
                        sections.haveFace ? " - using the nearest face instead"
                                          : " - this player will render with no face");
        }
    }

    // Composite base skin + face + underwear overlays
    rendering::VkTexture* compositeTex = nullptr;
    {
        std::vector<std::string> layers;
        layers.push_back(bodySkinPath);
        if (!faceLowerPath.empty()) layers.push_back(faceLowerPath);
        if (!faceUpperPath.empty()) layers.push_back(faceUpperPath);
        for (const auto& up : underwearPaths) layers.push_back(up);
        if (layers.size() > 1) {
            compositeTex = charRenderer->compositeTextures(layers);
        } else {
            compositeTex = charRenderer->loadTexture(bodySkinPath);
        }
    }

    rendering::VkTexture* hairTex = nullptr;
    if (!hairTexturePath.empty()) {
        hairTex = charRenderer->loadTexture(hairTexturePath);
    }
    // Texture type 8 is Skin Extra: the head detail sheet an HD model draws its
    // ears, eyes and eyelashes from. CharSections names it in the skin row's
    // second texture, which the tables the game shipped leave blank - so on a
    // stock model this still falls through to the underwear art it always used.
    rendering::VkTexture* skinExtraTex = nullptr;
    if (!skinExtraPath.empty()) skinExtraTex = charRenderer->loadTexture(skinExtraPath);
    else if (!underwearPaths.empty()) skinExtraTex = charRenderer->loadTexture(underwearPaths[0]);
    else skinExtraTex = charRenderer->loadTexture(pelvisPath);

    const PlayerTextureSlots& slots = playerTextureSlotsByModelId_[modelId];
    if (slots.skin >= 0 && compositeTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skin), compositeTex);
    }
    if (slots.hair >= 0 && hairTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.hair), hairTex);
    }
    if (slots.skinExtra >= 0 && skinExtraTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skinExtra), skinExtraTex);
    }

    // The component's geosets before the equipment is known: the hair, the
    // facial rows and the bare defaults (0x004ee460, 0x004ed900).
    {
        const core::CharacterLook component = playerLook(guid, raceId, genderId, appearanceBytes, facialFeatures);
        charRenderer->setActiveGeosets(
            instanceId, core::modelGeosetsShown(core::characterLookGeosets(*assetManager_, component),
                                                modelSubmeshIds(modelId)));
    }

    if (const auto pose = corpsePose(guid); pose && corpseGuids_.count(guid) &&
        charRenderer->hasAnimation(instanceId, rendering::anim::DEAD)) {
        // A corpse lies in Dead, or Drowned under water (0x00705b20).
        charRenderer->playAnimation(instanceId,
                                    charRenderer->hasAnimation(instanceId, *pose) ? *pose : rendering::anim::DEAD, true);
    } else if (deadCreatureGuids_.count(guid)) {
        charRenderer->playAnimation(instanceId, rendering::anim::DEATH, false);
    } else {
        // A player already seated when we first see them stays seated.
        uint32_t idleAnim = rendering::anim::STAND;
        if (gameHandler_) {
            auto entity = gameHandler_->getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::PLAYER) {
                idleAnim = rendering::anim::standStateAnims(
                    std::static_pointer_cast<game::Unit>(entity)->getStandState()).loop;
                if (!charRenderer->hasAnimation(instanceId, idleAnim))
                    idleAnim = rendering::anim::STAND;
            }
        }
        if (idleAnim != rendering::anim::STAND) charRenderer->setRestAnimation(instanceId, idleAnim);
        charRenderer->playAnimation(instanceId, idleAnim, true);
    }
    playerInstances_[guid] = instanceId;

    // The mount field may have arrived before this render instance, or the
    // player may be re-created without another values update. Reconcile from
    // retained entity state so already-mounted players are never left on foot.
    if (gameHandler_) {
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
        if (unit && unit->getMountDisplayId() != 0) {
            setRemotePlayerMountDisplayId(guid, unit->getMountDisplayId());
        }
    }

    OnlinePlayerAppearanceState st;
    st.instanceId = instanceId;
    st.modelId = modelId;
    st.raceId = raceId;
    st.genderId = genderId;
    st.appearanceBytes = appearanceBytes;
    st.facialFeatures = facialFeatures;
    st.bodySkinPath = bodySkinPath;
    // Include face textures so compositeWithRegions can rebuild the full base
    if (!faceLowerPath.empty()) st.underwearPaths.push_back(faceLowerPath);
    if (!faceUpperPath.empty()) st.underwearPaths.push_back(faceUpperPath);
    for (const auto& up : underwearPaths) st.underwearPaths.push_back(up);
    onlinePlayerAppearance_[guid] = std::move(st);
}

void EntitySpawner::setOnlinePlayerEquipment(uint64_t guid,
                                          const std::array<uint32_t, 19>& displayInfoIds,
                                          const std::array<uint8_t, 19>& inventoryTypes) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;

    // Skip local player - equipment handled by GameScreen::updateCharacterGeosets/Textures
    // via consumeOnlineEquipmentDirty(), which fires on the same server update.
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        if (localGuid != 0 && guid == localGuid) return;
    }

    // If the player isn't spawned yet, store equipment until spawn.
    auto appIt = onlinePlayerAppearance_.find(guid);
    if (!playerInstances_.count(guid) || appIt == onlinePlayerAppearance_.end()) {
        pendingOnlinePlayerEquipment_[guid] = {displayInfoIds, inventoryTypes};
        return;
    }

    const OnlinePlayerAppearanceState& st = appIt->second;

    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    if (st.instanceId == 0 || st.modelId == 0) return;

    if (st.bodySkinPath.empty()) {
        LOG_DEBUG("setOnlinePlayerEquipment: bodySkinPath empty for guid=0x", std::hex, guid, std::dec,
                    " instanceId=", st.instanceId, " - skipping equipment");
        return;
    }

    int nonZeroDisplay = 0;
    for (uint32_t d : displayInfoIds) if (d != 0) nonZeroDisplay++;
    LOG_DEBUG("setOnlinePlayerEquipment: guid=0x", std::hex, guid, std::dec,
                " instanceId=", st.instanceId, " nonZeroDisplayIds=", nonZeroDisplay,
                " head=", displayInfoIds[0], " chest=", displayInfoIds[4],
                " legs=", displayInfoIds[6], " mainhand=", displayInfoIds[15]);

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

    auto hasInvType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return true;
            }
        }
        return false;
    };

    // --- Geosets ---
    // The client's character component (0x004ed900): the hair and facial
    // rows' defaults, the helmet's HelmetGeosetVisData masks, and what each
    // worn item's GeosetGroup columns add and take away. By equipment slot:
    // 0 head, 3 shirt, 4 chest, 5 waist, 6 legs, 7 feet, 9 hands, 14 back,
    // 18 tabard.
    core::CharacterLook component = playerLook(guid, st.raceId, st.genderId, st.appearanceBytes,
                                               st.facialFeatures);
    component.worn.head = displayInfoIds[0];
    component.worn.shirt = displayInfoIds[3];
    component.worn.chest = displayInfoIds[4];
    component.worn.belt = displayInfoIds[5];
    component.worn.legs = displayInfoIds[6];
    component.worn.boots = displayInfoIds[7];
    component.worn.gloves = displayInfoIds[9];
    component.worn.cape = displayInfoIds[14];
    component.worn.tabard = displayInfoIds[18];
    charRenderer->setActiveGeosets(
        st.instanceId, core::modelGeosetsShown(core::characterLookGeosets(*assetManager_, component),
                                               modelSubmeshIds(st.modelId)));

    // --- Helmet model attachment ---
    // HEAD slot is index 0 in the 19-element equipment array.
    // Helmet M2s are race/gender-specific (e.g. Helm_Plate_B_01_HuM.m2 for Human Male).
    if (displayInfoIds[0] != 0) {
        // Only the helm point - detaching 0 as well would drop the shield.
        charRenderer->detachWeapon(st.instanceId, kAttachHelm);

        const core::HelmVisual helm = core::resolveHelmVisual(
            *assetManager_, displayInfoIds[0], st.raceId, st.genderId);
        if (helm.valid()) {
            pipeline::M2Model helmModel;
            std::string helmPath;
            if (!helm.racialModelPath.empty()) {
                helmPath = helm.racialModelPath;
                if (!loadWeaponM2(helmPath, helmModel)) helmModel = {};
            }
            if (!helmModel.isValid()) {
                helmPath = helm.baseModelPath;
                loadWeaponM2(helmPath, helmModel);
            }

            if (helmModel.isValid()) {
                const uint32_t helmModelId = nextWeaponModelId_++;
                // Attachment point 0 (head bone), fallback to 11 (explicit head).
                const bool attached = charRenderer->attachWeapon(
                    st.instanceId, kAttachHelm, helmModel, helmModelId, helm.texturePath);
                if (attached) {
                    LOG_DEBUG("Attached player helmet: ", helmPath, " tex: ", helm.texturePath);
                }
            }
        }
    } else {
        // No helmet equipped - detach any existing helmet model
        charRenderer->detachWeapon(st.instanceId, kAttachHelm);
    }

    // --- Shoulder models (0x004ef840): the display's first model on the left
    // shoulder, its second on the right. Slot 2 of the equipment array.
    core::attachShoulders(*charRenderer, *assetManager_, st.instanceId, displayInfoIds[2],
                          [this] { return nextWeaponModelId_++; });

    // --- Cape texture (group 15 / texture type 2) ---
    // The geoset above enables the cape mesh, but without a texture it renders blank.
    if (hasInvType({16})) {
        // Back/cloak is WoW equipment slot 14 (BACK) in the 19-element array.
        uint32_t capeDid = displayInfoIds[14];
        if (capeDid != 0) {
            int32_t capeRecIdx = displayInfoDbc->findRecordById(capeDid);
            if (capeRecIdx >= 0) {
                const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                // RightModelTexture is the field right after LeftModelTexture.
                // Some cloaks (e.g. Jaina's Radiance) carry their texture only in
                // the right field; the character-preview screen checks both, so
                // match it here - otherwise the world model shows the cape mesh
                // untextured even though the paperdoll preview looks correct.
                const uint32_t rightTexField = leftTexField + 1;
                std::string leftName = displayInfoDbc->getString(
                    static_cast<uint32_t>(capeRecIdx), leftTexField);
                std::string rightName = displayInfoDbc->getString(
                    static_cast<uint32_t>(capeRecIdx), rightTexField);

                std::vector<std::string> capeNames;
                auto addCapeName = [&](const std::string& n) {
                    if (!n.empty() &&
                        std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end())
                        capeNames.push_back(n);
                };
                if (st.genderId == 1) { addCapeName(rightName); addCapeName(leftName); }
                else                  { addCapeName(leftName);  addCapeName(rightName); }

                if (!capeNames.empty()) {
                    // Where a cape's art might be, in the order to try it -
                    // pipeline/item_textures.hpp. Written out here, in the NPC
                    // path and in the portrait, identically, which is the only
                    // reason the three agreed.
                    std::vector<std::string> capeCandidates;
                    for (const auto& capeName : capeNames) {
                        for (auto& c : pipeline::capeTextureCandidates(capeName, st.genderId == 1)) {
                            if (std::find(capeCandidates.begin(), capeCandidates.end(), c) ==
                                capeCandidates.end()) {
                                capeCandidates.push_back(std::move(c));
                            }
                        }
                    }

                    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    rendering::VkTexture* capeTexture = nullptr;
                    for (const auto& candidate : capeCandidates) {
                        rendering::VkTexture* tex = charRenderer->loadTexture(candidate);
                        if (tex && tex != whiteTex) {
                            capeTexture = tex;
                            break;
                        }
                    }

                    if (capeTexture) {
                        charRenderer->setGroupTextureOverride(st.instanceId, 15, capeTexture);
                        if (const auto* md = charRenderer->getModelData(st.modelId)) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 2) {
                                    charRenderer->setTextureSlotOverride(
                                        st.instanceId, static_cast<uint16_t>(ti), capeTexture);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Textures (skin atlas compositing) ---

    // The character component's layers (0x004f2880), and a guild tabard's
    // emblem from its guild's design (0x006db510; a corpse's guild,
    // 0x007059a0) - asked for, and painted again on its answer.
    std::vector<core::ComponentItem> componentItems;
    for (int s = 0; s < 19; s++) {
        if (displayInfoIds[s] != 0) componentItems.push_back({core::componentItemIndex(s), displayInfoIds[s]});
    }
    std::optional<game::GuildEmblem> emblem;
    paintedTabards_.erase(guid);
    if (displayInfoIds[18] != 0 && gameHandler_) {
        const auto siteIt = corpseSites_.find(guid);
        const uint32_t guildId =
            siteIt != corpseSites_.end() ? siteIt->second.guildId : gameHandler_->getEntityGuildId(guid);
        emblem = gameHandler_->lookupGuildEmblem(guildId);
        // Painted again on a change of its guild or its guild's design
        // (refreshGuildTabards).
        paintedTabards_[guid] = {.displayIds = displayInfoIds,
                                 .inventoryTypes = inventoryTypes,
                                 .guildId = guildId,
                                 .emblem = emblem};
    }
    const std::vector<std::pair<int, std::string>> regionLayers = core::characterComponentLayers(
        *assetManager_, *displayInfoDbc, componentItems, st.genderId == 1, emblem);

    const auto slotsIt = playerTextureSlotsByModelId_.find(st.modelId);
    if (slotsIt == playerTextureSlotsByModelId_.end()) return;
    const PlayerTextureSlots& slots = slotsIt->second;
    if (slots.skin < 0) return;

    rendering::VkTexture* newTex = charRenderer->compositeWithRegions(st.bodySkinPath, st.underwearPaths, regionLayers);
    if (newTex) {
        charRenderer->setTextureSlotOverride(st.instanceId, static_cast<uint16_t>(slots.skin), newTex);
    }

    // The weapons follow the visible item entries and the sheath state:
    // updateUnitWeapons (0x0072dbc0).
}

void EntitySpawner::despawnPlayer(uint64_t guid) {
    // A player still waiting on its model leaves the queue too. The wait can now
    // be the length of a model load, and one that walked out of range during it
    // was otherwise drawn when the load finished, where it was last seen.
    if (pendingPlayerSpawnGuids_.erase(guid) > 0) {
        pendingPlayerSpawns_.erase(
            std::remove_if(pendingPlayerSpawns_.begin(), pendingPlayerSpawns_.end(),
                           [guid](const PendingPlayerSpawn& p) { return p.guid == guid; }),
            pendingPlayerSpawns_.end());
    }
    pendingOnlinePlayerEquipment_.erase(guid);
    unitWeaponsShown_.erase(guid);
    if (!renderer_ || !renderer_->getCharacterRenderer()) return;
    pendingRemotePlayerMounts_.erase(guid);
    removeRemotePlayerMount(guid);
    auto it = playerInstances_.find(guid);
    if (it == playerInstances_.end()) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    // The race and gender models stay loaded: they are cached in
    // playerModelCache_ with every animation read in, which is a load worth
    // keeping for the next player of that race. Anything else the instance was
    // drawn with goes with it.
    const uint32_t instanceModelId = charRenderer->getInstanceModelId(it->second);
    charRenderer->removeInstance(it->second);
    const bool isCachedPlayerModel = std::any_of(
        playerModelCache_.begin(), playerModelCache_.end(),
        [instanceModelId](const auto& entry) { return entry.second == instanceModelId; });
    if (instanceModelId != 0 && !isCachedPlayerModel) charRenderer->unloadModelIfUnused(instanceModelId);
    playerInstances_.erase(it);
    onlinePlayerAppearance_.erase(guid);
    deadCreatureGuids_.erase(guid);
    creatureRenderPosCache_.erase(guid);
    creatureSwimmingState_.erase(guid);
    creatureWalkingState_.erase(guid);
    creatureFlyingState_.erase(guid);
    creatureWasMoving_.erase(guid);
    creatureWasSwimming_.erase(guid);
    creatureWasFlying_.erase(guid);
    creatureWasWalking_.erase(guid);
}

// ---------------------------------------------------------------------------
// Which hull a transport is drawn with
//
// Entry and displayId are different numbering spaces and this table used to mix
// them, which is how an elevator became a zeppelin. Every ship and zeppelin
// entry here shares one of five displayIds -- 3015, 3031, 7087, 7446, 7546 --
// and the numbers that were being compared against displayId were entries:
// 164871, 175080 and 176495 are the three vanilla zeppelins, and no displayId
// reaches six figures, so those three could never match.
//
// What did match was worse. 807 and 808 are the displayIds of Gnomeregan lifts
// (the "Vator" and the "Plunger"), 2454 belongs to the Searing Gorge scaffold
// cars and 1587 to a GameObject named, plainly, "Elevator" -- so every one of
// them was being drawn as an airship. Elevators are transports too, which is
// why the caller's guard lets them in.
//
// Values verified against gameobject_template.
std::string EntitySpawner::transportModelPath(uint32_t entry, uint32_t displayId) {
    struct TransportModel { uint32_t entry; uint32_t displayId; const char* path; };
    static constexpr const char* kShip     = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
    static constexpr const char* kZeppelin = "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo";
    static constexpr const char* kHordeZep = "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo";
    static constexpr const char* kIceship  = "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo";
    // NOLINTBEGIN(modernize-use-designated-initializers) - a table whose
    // columns are its field names, with the struct in view directly above.
    static constexpr TransportModel kTransportModels[] = {
        // Ships (display 3015)
        {  20808, 3015, kShip },      // The Maiden's Fancy
        { 176231, 3015, kShip },      // The Lady Mehley
        { 176310, 3015, kShip },      // The Bravery
        // Zeppelins (display 3031)
        { 164871, 3031, kZeppelin },  // The Thundercaller
        { 175080, 3031, kZeppelin },  // The Iron Eagle
        { 176495, 3031, kZeppelin },  // The Purple Princess
        { 186371, 3031, kZeppelin },
        { 190549, 3031, kZeppelin },  // The Zephyr
        // Horde zeppelins (display 7546)
        { 181689, 7546, kHordeZep },  // Cloudkisser
        { 186238, 7546, kHordeZep },  // The Mighty Wind
        { 201834, 7546, kHordeZep },
        // Icebreakers (display 7446)
        { 181688, 7446, kIceship },   // Northspear
        { 190536, 7446, kIceship },   // Stormwind's Pride
    };
    // NOLINTEND(modernize-use-designated-initializers)

    for (const TransportModel& t : kTransportModels) {
        if (entry == t.entry || displayId == t.displayId) return t.path;
    }
    // The Deeprun Tram car, which is an M2 rather than a WMO and is keyed on a
    // displayId that is genuinely a displayId: entries 176080-176086 all carry
    // 3831.
    if (displayId == 3831) {
        return "World\\Generic\\Gnome\\Passive Doodads\\Subway\\SubwayCar.m2";
    }
    return "";
}

void EntitySpawner::spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer_ || !assetManager_) return;

    if (!gameObjectLookupsBuilt_) {
        buildGameObjectDisplayLookups();
    }
    if (!gameObjectLookupsBuilt_) return;

    LOG_DEBUG("GO spawn attempt: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " entry=", entry,
             " pos=(", x, ", ", y, ", ", z, ")");

    auto goIt = gameObjectInstances_.find(guid);
    if (goIt != gameObjectInstances_.end()) {
        // A tracked instance ID is only meaningful while the renderer still holds
        // it. Renderer-wide clears (map change, device reset) drop instances
        // without going through despawnGameObject(), which used to leave this map
        // pointing at a dead handle - every later server CREATE for that GUID then
        // took the position-update path below and the object stayed invisible for
        // the rest of the session. Treat a dead handle as "not spawned".
        bool instanceAlive = false;
        if (renderer_) {
            if (goIt->second.isWmo) {
                auto* wr = renderer_->getWMORenderer();
                instanceAlive = wr && wr->hasInstance(goIt->second.instanceId);
            } else {
                auto* mr = renderer_->getM2Renderer();
                instanceAlive = mr && mr->hasInstance(goIt->second.instanceId);
            }
        }
        if (!instanceAlive) {
            LOG_WARNING("GO render instance vanished - respawning: guid=0x", std::hex, guid, std::dec,
                        " displayId=", displayId, " instanceId=", goIt->second.instanceId);
            gameObjectInstances_.erase(goIt);
            goIt = gameObjectInstances_.end();
            // The new instance takes the pose the old one was in.
            if (auto st = gameObjectServerState_.find(guid); st != gameObjectServerState_.end()) {
                gameObjectPendingState_[guid] = st->second;
                gameObjectServerState_.erase(st);
            }
        }
    }
    if (goIt != gameObjectInstances_.end()) {
        if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
            if (auto* transportManager = gameHandler_->getTransportManager()) {
                if (transportManager->getTransport(guid)) {
                    transportManager->rebindTransportInstance(
                        guid, goIt->second.instanceId, !goIt->second.isWmo, displayId);
                    transportManager->updateServerTransport(
                        guid, glm::vec3(x, y, z), orientation);
                } else {
                    gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
                }
            } else {
                gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }
            return;
        }

        // Already have a render instance - update its position (e.g. transport re-creation)
        auto& info = goIt->second;
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        LOG_DEBUG("GameObject position update: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
                 " pos=(", x, ", ", y, ", ", z, ")");
        if (renderer_) {
            if (info.isWmo) {
                if (auto* wr = renderer_->getWMORenderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                    wr->setInstanceTransform(info.instanceId, transform);
                }
            } else {
                if (auto* mr = renderer_->getM2Renderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    mr->setInstanceTransform(info.instanceId, transform);
                }
            }
        }
        return;
    }

    std::string modelPath;

    if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
        modelPath = transportModelPath(entry, displayId);
        if (!modelPath.empty()) {
            LOG_INFO("Transport entry/display ", entry, "/", displayId, " → ", modelPath);
        }
    }

    // Fallback to normal displayId lookup if not a transport or no override matched
    if (modelPath.empty()) {
        modelPath = getGameObjectModelPathForDisplayId(displayId);
    }

    if (modelPath.empty()) {
        LOG_WARNING("No model path for gameobject displayId ", displayId, " (guid 0x", std::hex, guid, std::dec, ")");
        return;
    }

    // Log spawns to help debug duplicate objects (e.g., cathedral issue)
    LOG_DEBUG("GameObject spawn: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
             " model=", modelPath, " pos=(", x, ", ", y, ", ", z, ")");

    std::string lowerPath = modelPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool isWmo = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".wmo";

    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    const float renderYawWmo = orientation;
    // M2 game objects: model default faces +renderX. renderYaw = canonical + 90° = server_yaw
    // (same offset as creature/character renderer_ so all M2 models face consistently)
    const float renderYawM2go = orientation + glm::radians(90.0f);

    bool loadedAsWmo = false;
    if (isWmo) {
        auto* wmoRenderer = renderer_->getWMORenderer();
        if (!wmoRenderer) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdWmoCache_.find(displayId);
        if (itCache != gameObjectDisplayIdWmoCache_.end()) {
            modelId = itCache->second;
            // Only use cached entry if the model is still resident in the renderer_
            if (wmoRenderer->isModelLoaded(modelId)) {
                loadedAsWmo = true;
            } else {
                gameObjectDisplayIdWmoCache_.erase(itCache);
                modelId = 0;
            }
        }
        if (!loadedAsWmo && modelId == 0) {
            auto wmoData = assetManager_->readFile(modelPath);
            if (!wmoData.empty()) {
                pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
                LOG_DEBUG("Gameobject WMO root loaded: ", modelPath, " nGroups=", wmoModel.nGroups);
                int loadedGroups = 0;
                if (wmoModel.nGroups > 0) {
                    for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                        bool loaded = false;
                        for (const std::string& groupPath :
                             pipeline::wmoGroupCandidates(modelPath, gi)) {
                            std::vector<uint8_t> groupData =
                                assetManager_->readFile(groupPath);
                            if (groupData.empty()) continue;
                            pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                            loadedGroups++;
                            loaded = true;
                            break;
                        }
                        if (!loaded) {
                            LOG_WARNING("  Failed to load WMO group ", gi, " for: ", modelPath);
                        }
                    }
                }

                if (loadedGroups > 0 || wmoModel.nGroups == 0) {
                    modelId = nextGameObjectWmoModelId_++;
                    if (wmoRenderer->loadModel(wmoModel, modelId)) {
                        gameObjectDisplayIdWmoCache_[displayId] = modelId;
                        loadedAsWmo = true;
                    } else {
                        LOG_WARNING("Failed to load gameobject WMO model: ", modelPath);
                    }
                } else {
                    LOG_WARNING("No WMO groups loaded for gameobject: ", modelPath,
                                " - falling back to M2");
                }
            } else {
                LOG_WARNING("Failed to read gameobject WMO: ", modelPath, " - falling back to M2");
            }
        }

        if (loadedAsWmo) {
            uint32_t instanceId = wmoRenderer->createInstance(modelId, renderPos,
                glm::vec3(0.0f, 0.0f, renderYawWmo), scale);
            if (instanceId == 0) {
                LOG_WARNING("Failed to create gameobject WMO instance for guid 0x", std::hex, guid, std::dec);
                return;
            }

            gameObjectInstances_[guid] = {.modelId = modelId, .instanceId = instanceId, .isWmo = true};
            LOG_DEBUG("Spawned gameobject WMO: guid=0x", std::hex, guid, std::dec,
                     " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");

            // Spawn transport WMO doodads (chairs, furniture, etc.) as child M2 instances
            bool isTransport = false;
            if (gameHandler_) {
                std::string lowerModelPath = modelPath;
                std::transform(lowerModelPath.begin(), lowerModelPath.end(), lowerModelPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                isTransport = (lowerModelPath.find("transport") != std::string::npos);
            }

            auto* m2Renderer = renderer_->getM2Renderer();
            if (m2Renderer && isTransport) {
                const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(modelId);
                if (doodadTemplates && !doodadTemplates->empty()) {
                    constexpr size_t kMaxTransportDoodads = 192;
                    const size_t doodadBudget = std::min(doodadTemplates->size(), kMaxTransportDoodads);
                    LOG_DEBUG("Queueing ", doodadBudget, "/", doodadTemplates->size(),
                             " transport doodads for WMO instance ", instanceId);
                    pendingTransportDoodadBatches_.push_back(PendingTransportDoodadBatch{
                        guid,
                        modelId,
                        instanceId,
                        0,
                        doodadBudget,
                        0,
                        x, y, z,
                        orientation
                    });
                } else {
                LOG_DEBUG("Transport WMO has no doodads or templates not available");
            }
            }

            // Transport GameObjects are not always named "transport" in their WMO path
            // (e.g. elevators/lifts). If the server marks it as a transport, always
            // notify so TransportManager can animate/carry passengers.
            bool isTG = gameHandler_ && gameHandler_->isTransportGuid(guid);
            LOG_DEBUG("WMO GO spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " isTransport=", isTG,
                       " pos=(", x, ", ", y, ", ", z, ")");
            if (isTG) {
                gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }

            return;
        }

        // WMO failed - fall through to try as M2
        // Convert .wmo path to .m2 for fallback
        modelPath = modelPath.substr(0, modelPath.size() - 4) + ".m2";
    }

    {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (!m2Renderer) return;

        // Skip displayIds that permanently failed to load (e.g. empty/unsupported M2s).
        // Without this guard the same empty model is re-parsed every frame, causing
        // sustained log spam and wasted CPU.
        if (gameObjectDisplayIdFailedCache_.count(displayId)) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdModelCache_.find(displayId);
        if (itCache != gameObjectDisplayIdModelCache_.end()) {
            modelId = itCache->second;
            if (!m2Renderer->hasModel(modelId)) {
                LOG_WARNING("GO M2 cache hit but model gone: displayId=", displayId,
                            " modelId=", modelId, " path=", modelPath,
                            " - reloading");
                gameObjectDisplayIdModelCache_.erase(itCache);
                itCache = gameObjectDisplayIdModelCache_.end();
            }
        }
        if (itCache == gameObjectDisplayIdModelCache_.end()) {
            modelId = nextGameObjectModelId_++;

            auto m2Data = assetManager_->readFile(modelPath);
            if (m2Data.empty()) {
                LOG_WARNING("Failed to read gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
            // Collision classification needs the asset path. Embedded M2 names
            // are often generic and caused herb/grass gameobjects to be treated
            // as solid props.
            model.name = modelPath;
            if (model.vertices.empty()) {
                LOG_WARNING("Failed to parse gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty() && model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, model);
            } else if (skinData.empty() && model.version >= 264) {
                LOG_WARNING("GO skin file MISSING for WotLK M2 (no indices/batches): ", skinPath);
            }

            LOG_DEBUG("GO model: ", modelPath, " v=", model.version,
                     " verts=", model.vertices.size(),
                     " idx=", model.indices.size(),
                     " batches=", model.batches.size(),
                     " bones=", model.bones.size(),
                     " skin=", (skinData.empty() ? "MISSING" : "ok"));

            if (!m2Renderer->loadModel(model, modelId)) {
                LOG_WARNING("Failed to load gameobject model: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            // Keep game object models resident across the away-and-back cycle.
            // Leaving town drops every instance of them, and the 60s reaper then
            // evicted the model - the log showed PostBoxHuman.m2 (the mailbox)
            // going through exactly that reap/reload churn on every return trip.
            m2Renderer->setModelPinned(modelId, true);
            gameObjectDisplayIdModelCache_[displayId] = modelId;
        }

        uint32_t instanceId = m2Renderer->createInstance(modelId, renderPos,
            glm::vec3(0.0f, 0.0f, renderYawM2go), scale);
        if (instanceId == 0) {
            LOG_WARNING("Failed to create gameobject instance for guid 0x", std::hex, guid, std::dec);
            return;
        }

        // Server game objects are not doodads: the client's doodad size-class
        // distances (0x00791cb0) do not hold them.
        m2Renderer->setInstanceIsGameObject(instanceId, true);

        // Deeprun Tram cars: riding never used real mesh collision to begin with (Z is
        // fully code-locked to the transport's simulated position while boarded, not
        // derived from a floor query), so the solid SubwayCar.m2 body was only ever in
        // the way - reported live as getting physically stuck walking back across a car
        // after crossing it once. Skip collision so the model is purely visual/decorative
        // for movement purposes, matching how the boarding logic already treats it (a
        // proximity/footprint check, not a physical block).
        if (displayId == 3831u) {
            m2Renderer->setSkipCollision(instanceId, true);
        }

        gameObjectInstances_[guid] = {.modelId = modelId, .instanceId = instanceId, .isWmo = false};

        // ...and the pose the server described before this model existed.
        if (auto pending = gameObjectPendingState_.find(guid);
            pending != gameObjectPendingState_.end()) {
            applyGameObjectState(guid, pending->second);
        }

        // Notify transport system for M2 transports (e.g. Deeprun Tram cars)
        if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
            LOG_DEBUG("M2 transport spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " instanceId=", instanceId);
            gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
        }
    }

    LOG_DEBUG("Spawned gameobject: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

} // namespace core
} // namespace wowee
