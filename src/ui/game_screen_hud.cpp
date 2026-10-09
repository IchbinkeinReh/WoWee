#include "core/character_component.hpp"
#include "ui/game_screen.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/ui_texture_load.hpp"
#include "ui/ui_upload_budget.hpp"
#include "core/helm_visual.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/appearance_composer.hpp"
#include "ui/map_window.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "game/zone_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/geoset_rules.hpp"
#include "pipeline/item_textures.hpp"

#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>
#include <limits>

#include <unordered_set>
#include "ui/framexml_takeover.hpp"
#include "addons/lua_api_helpers.hpp"
#include "core/local_time.hpp"
#include "pipeline/spell_icon_paths.hpp"

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;



}

namespace wowee { namespace ui {



void GameScreen::updateCharacterGeosets(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    uint32_t instanceId = renderer->getCharacterInstanceId();
    if (instanceId == 0) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    auto* gh = app.getGameHandler();
    const game::Character* ch = gh ? gh->getActiveCharacter() : nullptr;
    if (!charRenderer || !appearanceComposer_ || !ch) return;

    // The display of the item worn in an equipment slot.
    auto worn = [&](game::EquipSlot slot) -> uint32_t {
        const auto& s = inventory.getEquipSlot(slot);
        return s.empty() ? 0 : s.item.displayInfoId;
    };

    // The client's character component (0x004ed900): the hair and facial
    // rows, the helmet's masks, and each worn item's geoset columns. A helm or
    // cloak the player has chosen not to show is not on the character at all.
    core::CharacterLook look = appearanceComposer_->playerLook(*ch);
    look.worn.head = gh->isHelmVisible() ? worn(game::EquipSlot::HEAD) : 0;
    look.worn.shirt = worn(game::EquipSlot::SHIRT);
    look.worn.chest = worn(game::EquipSlot::CHEST);
    look.worn.belt = worn(game::EquipSlot::WAIST);
    look.worn.legs = worn(game::EquipSlot::LEGS);
    look.worn.boots = worn(game::EquipSlot::FEET);
    look.worn.gloves = worn(game::EquipSlot::HANDS);
    look.worn.cape = gh->isCloakVisible() ? worn(game::EquipSlot::BACK) : 0;
    look.worn.tabard = worn(game::EquipSlot::TABARD);
    charRenderer->setActiveGeosets(instanceId, appearanceComposer_->playerGeosets(look, instanceId));
}

void GameScreen::updateCharacterTextures(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;

    auto* assetManager = app.getAssetManager();
    if (!assetManager) return;

    const auto& bodySkinPath = app.getBodySkinPath();
    const auto& underwearPaths = app.getUnderwearPaths();
    uint32_t skinSlot = app.getSkinTextureSlotIndex();

    if (bodySkinPath.empty()) return;

    // Component directory names indexed by region

    // Load ItemDisplayInfo.dbc
    auto displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;

    // The character component's layers (0x004f2880), with the guild
    // tabard's emblem from the guild's design (0x006db510).
    std::vector<core::ComponentItem> componentItems;
    for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
        const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (slot.empty() || slot.item.displayInfoId == 0) continue;
        componentItems.push_back({core::componentItemIndex(s), slot.item.displayInfoId});
    }
    bool isFemale = false;
    std::optional<game::GuildEmblem> emblem;
    if (auto* gh = app.getGameHandler()) {
        if (auto* ch = gh->getActiveCharacter()) {
            isFemale = (ch->gender == game::Gender::FEMALE) ||
                       (ch->gender == game::Gender::NONBINARY && ch->useFemaleModel);
        }
        playerTabardGuildId_ = gh->getEntityGuildId(gh->getPlayerGuid());
        if (playerTabardGuildId_ == 0 && gh->getActiveCharacter()) playerTabardGuildId_ = gh->getActiveCharacter()->guildId;
        emblem = gh->lookupGuildEmblem(playerTabardGuildId_);
        playerTabardEmblem_ = emblem;
    }
    const std::vector<std::pair<int, std::string>> regionLayers =
        core::characterComponentLayers(*assetManager, *displayInfoDbc, componentItems, isFemale, emblem);

    // Re-composite: base skin + underwear + equipment regions
    // Clear composite cache first to prevent stale textures from being reused
    charRenderer->clearCompositeCache();
    // Use per-instance texture override (not model-level) to avoid deleting cached composites.
    uint32_t instanceId = renderer->getCharacterInstanceId();
    auto* newTex = charRenderer->compositeWithRegions(bodySkinPath, underwearPaths, regionLayers);
    if (newTex != nullptr && instanceId != 0) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(skinSlot), newTex);
    }

    // Cloak cape texture - separate from skin atlas, uses texture slot type-2 (Object Skin)
    uint32_t cloakSlot = app.getCloakTextureSlotIndex();
    if (cloakSlot > 0 && instanceId != 0) {
        // Find equipped cloak (inventoryType 16)
        uint32_t cloakDisplayId = 0;
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty() && slot.item.inventoryType == 16 && slot.item.displayInfoId != 0) {
                cloakDisplayId = slot.item.displayInfoId;
                break;
            }
        }

        if (cloakDisplayId > 0) {
            int32_t recIdx = displayInfoDbc->findRecordById(cloakDisplayId);
            if (recIdx >= 0) {
                // DBC field 3 = modelTexture_1 (cape texture name)
                const auto* dispL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                std::string capeName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), dispL ? (*dispL)["LeftModelTexture"] : 3);
                if (!capeName.empty()) {
                    // This asked for one path only - ObjectComponents, no
                    // suffix - and a cape whose art is filed anywhere else
                    // showed white. The full list, in order, is in
                    // pipeline/item_textures.hpp, and the other three places
                    // that load a cape have always used all of it.
                    bool isFemale = false;
                    if (auto* gh = app.getGameHandler()) {
                        if (auto* ch = gh->getActiveCharacter()) {
                            isFemale = (ch->gender == game::Gender::FEMALE) ||
                                       (ch->gender == game::Gender::NONBINARY && ch->useFemaleModel);
                        }
                    }
                    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    for (const auto& capePath : pipeline::capeTextureCandidates(capeName, isFemale)) {
                        auto* capeTex = charRenderer->loadTexture(capePath);
                        if (capeTex == nullptr || capeTex == whiteTex) continue;
                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot), capeTex);
                        LOG_INFO("Cloak texture applied: ", capePath);
                        break;
                    }
                }
            }
        } else {
            // No cloak equipped - clear override so model's default (white) shows
            charRenderer->clearTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot));
        }
    }
}

// ============================================================
// World Map
// ============================================================

// Everything the map shows besides the land: the zone the player is in, what
// they have explored, their party, flight points, quests, their corpse and the
// rares nearby. For the in-game map and the second-window one alike, which is
// why it is on its own - the second window's map is fed every frame it is open,
// whatever the in-game one is doing.
void GameScreen::feedWorldMap(game::GameHandler& gameHandler,
                              rendering::world_map::WorldMapFacade& targetMap,
                              const std::function<bool(uint32_t)>& questAreaShown) {
    auto* renderer = core::Application::getInstance().getRenderer();
    if (!renderer) return;
    auto* wm = &targetMap;

    // Keep map name in sync with minimap's map name
    auto* minimap = renderer->getMinimap();
    if (minimap) {
        wm->setMapName(minimap->getMapName());
    }
    wm->setServerExplorationMask(
        gameHandler.getPlayerExploredZoneMasks(),
        gameHandler.hasPlayerExploredZoneMasks());
    // Which zone the player is actually in, rather than which WorldMapArea box
    // they happen to sit deepest inside. The boxes are axis-aligned rectangles
    // around irregular zones and overlap their neighbours heavily, so opening
    // the map could land on a zone the player was only near.
    //
    // The zone under the player's feet first, as GetZoneText has it, and the
    // server's only where the terrain cannot say - inside an instance, or
    // before the first chunk is in. The server names a zone on
    // SMSG_INIT_WORLD_STATES alone, so its answer is the last zone it noticed,
    // and the map opened on the zone the player had just walked out of.
    const uint32_t liveZone = renderer->getCurrentZoneId();
    wm->setPlayerZoneId(liveZone != 0 ? liveZone : gameHandler.getWorldStateZoneId());

    // Party member dots on world map
    {
        std::vector<rendering::WorldMapPartyDot> dots;
        if (gameHandler.isInGroup()) {
            const auto& partyData = gameHandler.getPartyData();
            for (const auto& member : partyData.members) {
                if (!member.isOnline || !member.hasPartyStats) continue;
                if (member.posX == 0 && member.posY == 0) continue;
                // posY → canonical X (north), posX → canonical Y (west)
                float wowX = static_cast<float>(member.posY);
                float wowY = static_cast<float>(member.posX);
                glm::vec3 rpos = core::coords::canonicalToRender(glm::vec3(wowX, wowY, 0.0f));
                auto ent = gameHandler.getEntityManager().getEntity(member.guid);
                uint8_t cid = entityClassId(ent.get());
                ImU32 col = (cid != 0)
                    ? classColorU32(cid, 230)
                    : (member.guid == partyData.leaderGuid
                       ? IM_COL32(255, 210, 0, 230)
                       : IM_COL32(100, 180, 255, 230));
                dots.push_back({ .renderPos = rpos, .color = col, .name = member.name });
            }
        }
        // Battleground team positions, which this client had only ever drawn
        // on the minimap.
        //
        // FrameXML draws them on its own world map - WorldMapRaid1..40, placed
        // from GetNumBattlefieldPositions and GetBattlefieldPosition - but that
        // is exactly the area this client's map surface covers, so nothing
        // FrameXML puts there can be seen. Handing the map over made the
        // interface responsible for a layer it cannot show, so the surface
        // that hides it has to draw them instead.
        //
        // The same two group colours the minimap uses, so a flag carrier is
        // the same colour on both.
        {
            for (const auto& bp : gameHandler.getBgPlayerPositions()) {
                // Packet coords are canonical: wowX north, wowY west.
                const glm::vec3 rpos =
                    core::coords::canonicalToRender(glm::vec3(bp.wowX, bp.wowY, 0.0f));
                dots.push_back({ .renderPos = rpos, .color = ui::bgGroupColor(bp.group),
                                 .name = gameHandler.lookupName(bp.guid) });
            }
        }
        wm->setPartyDots(std::move(dots));
    }

    // Taxi node markers on world map
    {
        std::vector<rendering::WorldMapTaxiNode> taxiNodes;
        const auto& nodes = gameHandler.getTaxiNodes();
        uint32_t currentTaxiNode = gameHandler.getTaxiCurrentNode();
        const bool playerAlliance = gameHandler.isPlayerAlliance();
        taxiNodes.reserve(nodes.size());
        for (const auto& [id, node] : nodes) {
            const bool known = gameHandler.isKnownTaxiNode(id);
            // Undiscovered nodes are shown so the player can see where flight
            // paths exist, but only ones their faction can actually use. A node's
            // faction is inferred from which taxi mount TaxiNodes.dbc lists:
            // own-faction mount → show; both mounts → neutral flight point, show;
            // opposite-faction-only OR no mount at all (boat/zeppelin/script
            // nodes) → hide. Known nodes are always shown.
            if (!known) {
                const bool hasAlliance = node.mountDisplayIdAlliance != 0;
                const bool hasHorde    = node.mountDisplayIdHorde != 0;
                const bool bothFactions = hasAlliance && hasHorde;   // neutral hub
                const bool ownFaction   = playerAlliance ? hasAlliance : hasHorde;
                if (!bothFactions && !ownFaction) continue;
            }
            rendering::WorldMapTaxiNode wtn;
            wtn.id    = node.id;
            wtn.mapId = node.mapId;
            // TaxiNodes.dbc stores server/wire-order coordinates - convert to
            // canonical (X=north, Y=west) like the taxi flight path code does,
            // or the markers land transposed on the map.
            glm::vec3 canonical = core::coords::serverToCanonical(
                glm::vec3(node.x, node.y, node.z));
            wtn.wowX  = canonical.x;
            wtn.wowY  = canonical.y;
            wtn.wowZ  = canonical.z;
            wtn.name  = node.name;
            wtn.known = known;
            wtn.costCopper = gameHandler.getTaxiCostTo(id);
            wtn.current    = (id == currentTaxiNode);
            wtn.reachable  = gameHandler.hasTaxiRouteTo(id);
            taxiNodes.push_back(std::move(wtn));
        }
        wm->setTaxiNodes(std::move(taxiNodes));
    }

    // Quest objective and quest-giver markers on the world map.
    {
        std::vector<rendering::WorldMap::QuestPoi> qpois;
        const auto& questStatuses = gameHandler.getNpcQuestStatuses();
        // Add authoritative NPC statuses first. Some servers also emit a
        // generic POI at the NPC's position; that duplicate is filtered below
        // so it cannot leave a teal objective circle on a quest giver.
        for (const auto& [guid, status] : questStatuses) {
            auto entity = gameHandler.getEntityManager().getEntity(guid);
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;

            rendering::WorldMap::QuestPoi qp;
            qp.wowX = entity->getX();
            qp.wowY = entity->getY();
            qp.name = std::static_pointer_cast<game::Unit>(entity)->getName();
            const auto marker = game::questGiverMarker(status);
            if (!marker.symbol) continue;
            if (marker.symbol[0] == '!') {
                qp.kind = marker.dim ? rendering::WorldMap::QuestPoi::Kind::AVAILABLE_LOW
                                     : rendering::WorldMap::QuestPoi::Kind::AVAILABLE;
            } else {
                qp.kind = marker.dim ? rendering::WorldMap::QuestPoi::Kind::INCOMPLETE
                                     : rendering::WorldMap::QuestPoi::Kind::REWARD;
            }
            qpois.push_back(std::move(qp));
        }

        constexpr float kQuestGiverPoiMergeDistance = 15.0f;
        constexpr float kQuestGiverPoiMergeDistanceSq =
            kQuestGiverPoiMergeDistance * kQuestGiverPoiMergeDistance;
        // Which quests the player is actually on. A quest POI outlives the
        // quest that asked for it - the points are kept until the next answer
        // for that quest, and abandoning one never comes with an answer - so
        // without this an abandoned quest's objectives stay on the map.
        std::unordered_set<uint32_t> questsInLog;
        for (const auto& quest : gameHandler.getQuestLog()) {
            if (quest.questId != 0) questsInLog.insert(quest.questId);
        }
        size_t objectivePois = 0;
        for (const auto& poi : gameHandler.getGossipPois()) {
            // Keep ordinary gossip navigation POIs, and quest points for a
            // quest in the log.
            //
            // This asked isQuestShownOnMap, a per-quest opt-in set by a
            // checkbox in the client's own quest tracker. That tracker was
            // handed to FrameXML and the checkbox went with it, leaving three
            // readers of a set nothing could add to: no objective has been
            // drawn on either map since.
            if (poi.questObjectiveIndex != -2 && !questsInLog.count(poi.data)) {
                continue;
            }
            if (poi.questObjectiveIndex >= 0) ++objectivePois;
            bool duplicatesQuestGiver = false;
            for (const auto& existing : qpois) {
                if (existing.kind == rendering::WorldMap::QuestPoi::Kind::OBJECTIVE) continue;
                const float dx = existing.wowX - poi.x;
                const float dy = existing.wowY - poi.y;
                if (dx * dx + dy * dy <= kQuestGiverPoiMergeDistanceSq) {
                    duplicatesQuestGiver = true;
                    break;
                }
            }
            if (duplicatesQuestGiver) continue;

            rendering::WorldMap::QuestPoi qp;
            qp.wowX = poi.x;
            qp.wowY = poi.y;
            qp.name = poi.name;
            // The shaded area, for the quest the map has selected. Every
            // objective of that quest gets its own, which is what the real
            // client shades: DrawQuestBlob names a quest, not an objective.
            if (poi.questObjectiveIndex >= 0 && !poi.area.empty() &&
                questAreaShown(poi.data)) {
                qp.area.reserve(poi.area.size());
                for (const auto& pt : poi.area) qp.area.emplace_back(pt.first, pt.second);
            }
            if (poi.questObjectiveIndex == -1) {
                // A quest POI with no objective index is the quest endpoint,
                // not an objective area. Completed quests use a yellow ?,
                // while in-progress endpoints use a gray ?.
                qp.kind = rendering::WorldMap::QuestPoi::Kind::INCOMPLETE;
                for (const auto& quest : gameHandler.getQuestLog()) {
                    if (quest.questId != poi.data) continue;
                    if (quest.complete) {
                        qp.kind = rendering::WorldMap::QuestPoi::Kind::REWARD;
                    }
                    break;
                }
            }
            qpois.push_back(std::move(qp));
        }
        // Said once, when the map has quests to show objectives for and no
        // points to show: the server answers CMSG_QUEST_POI_QUERY from its own
        // quest_poi table, and an empty one looks exactly like a client that
        // is not drawing them.
        static bool reportedNoObjectivePois = false;
        if (objectivePois == 0 && !questsInLog.empty() && !reportedNoObjectivePois) {
            reportedNoObjectivePois = true;
            LOG_WARNING("World map: ", questsInLog.size(), " quest(s) in the log and no "
                        "objective points for any of them - the server sent no quest POI "
                        "data, so only quest givers and endpoints can be drawn");
        }
        wm->setQuestPois(std::move(qpois));
    }

    // Corpse marker: show skull X on world map when ghost with unclaimed corpse
    {
        float corpseCanX = 0.0f, corpseCanY = 0.0f;
        bool ghostWithCorpse = gameHandler.isPlayerGhost() &&
                               gameHandler.getCorpseCanonicalPos(corpseCanX, corpseCanY);
        glm::vec3 corpseRender = ghostWithCorpse
            ? core::coords::canonicalToRender(glm::vec3(corpseCanX, corpseCanY, 0.0f))
            : glm::vec3{};
        wm->setCorpsePos(ghostWithCorpse, corpseRender);

        // And where releasing would put them. Shown while dead either way:
        // before releasing it is the choice being offered, and after it is the
        // place to walk back from.
        uint32_t healerMap = 0;
        glm::vec3 healerCanonical(0.0f);
        const bool haveHealer = gameHandler.isPlayerDead() &&
                                gameHandler.getDeathReleaseLocation(healerMap, healerCanonical) &&
                                healerMap == gameHandler.getCurrentMapId();
        wm->setGraveyardPos(haveHealer,
                            haveHealer ? core::coords::canonicalToRender(healerCanonical)
                                       : glm::vec3{});
    }

    // Rare tracker: mark every spawned rare / rare-elite the client currently has loaded.
    // Entities only exist while near the player, so a marker means that rare is out now.
    // Opt-in via the Interface setting; when off, feed an empty list so markers clear.
    {
        std::vector<rendering::WorldMapRareMark> rares;
        if (settingsPanel_.showRareTracker_)
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            const int rank = gameHandler.getCreatureRank(unit->getEntry());
            if (rank != 2 && rank != 4) continue;      // 2 = Rare Elite, 4 = Rare
            if (unit->getHealth() == 0) continue;       // skip dead/looted rares
            rendering::WorldMapRareMark m;
            m.renderPos = core::coords::canonicalToRender(
                glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
            m.name = unit->getName();
            m.rank = rank;
            rares.push_back(std::move(m));
        }
        wm->setRares(std::move(rares));
    }

}

void GameScreen::renderTouchChatButton([[maybe_unused]] game::GameHandler& gameHandler) {
#ifdef __ANDROID__
    // The chat box opens on Enter or a slash, and a phone has neither key. The
    // on-screen keyboard comes up by itself once an edit box has focus (see
    // the text input handling in UIManager), so opening the box is all this
    // has to do. Hidden while something is being typed: the keyboard is up
    // then, and Back or the keyboard's own enter closes the box again.
    if (ImGui::GetIO().WantTextInput || interfaceTakingTypedInput()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float h = ImGui::GetFrameHeight() * 1.8f;
    const float w = h * 2.2f;
    const float margin = h * 0.4f;
    // Top centre: the unit frames are on the left, the minimap and the map's
    // close button on the right, and the movement stick takes the lower left.
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f, margin));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.3f);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(20, 20, 20, 150));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##TouchChat", nullptr, flags)) {
        if (ImGui::Button("Chat", ImVec2(w, h))) {
            gameHandler.runInterfaceCommand("ChatFrame_OpenChat(\"\")");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
#endif
}

void GameScreen::renderWorldMap(game::GameHandler& gameHandler) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    // The map on the second window, under its own ImGui context: a setter can
    // free a texture, and that goes back through the context that made it.
    if (auto* mapWindow = app.getMapWindow(); mapWindow && mapWindow->map()) {
        mapWindow->withContext([&] {
            feedWorldMap(gameHandler, *mapWindow->map(), [mapWindow](uint32_t questId) {
                return questId != 0 && questId == mapWindow->selectedQuest();
            });
        });
    }

    auto* wm = renderer->getWorldMap();
    if (!wm) return;

    // Flight master window drives the world map's flight-map (taxi selection)
    // mode: opening SMSG_SHOWTAXINODES opens the map, activating a flight or
    // closing the gossip closes it. A user-dismissed map (Escape / X) closes
    // the flight master window through the onClose handler.
    // Not while FrameXML is drawing the flight map itself. The legacy taxi
    // list a few lines up already stands aside for that element; this mode did
    // not, so talking to a flight master put both on screen at once - TaxiFrame
    // over this client's own map, each with its own set of pins.
    const bool taxiWanted = gameHandler.isTaxiWindowOpen() &&
                            !frameXmlOwns(UiElement::Taxi);
    if (taxiWanted && !wm->isTaxiMapOpen()) {
        auto* gh = &gameHandler;
        wm->openTaxiMap(
            [gh](uint32_t dest) { return gh->getTaxiRouteTo(dest); },
            [gh](uint32_t dest) { gh->activateTaxi(dest); },
            [gh]() { gh->closeTaxi(); });
    } else if (!taxiWanted && wm->isTaxiMapOpen()) {
        wm->closeTaxiMap();
    }

    // Who says the map is wanted depends on who owns it. FrameXML's world map
    // is a frame it shows and hides, and application.cpp gives this one that
    // frame's rect while it is visible - so a rect being set is the same
    // statement as showWorldMap_ is for this client's own window.
    const bool frameXmlDrivesMap = frameXmlOwns(UiElement::WorldMap);
    const bool wanted = frameXmlDrivesMap
        ? (wm->hasFrameRect() || wm->isTaxiMapOpen())
        : (showWorldMap_ || wm->isTaxiMapOpen());
    if (!wanted) return;

    feedWorldMap(gameHandler, *wm, [&gameHandler](uint32_t questId) {
        return gameHandler.isQuestBlobShown(questId);
    });

    glm::vec3 playerPos = renderer->getCharacterPosition();
    float playerYaw = renderer->getCharacterYaw();
    auto* window = app.getWindow();
    int screenW = window ? window->getWidth() : 1280;
    int screenH = window ? window->getHeight() : 720;
    wm->render(playerPos, screenW, screenH, playerYaw);

#ifdef __ANDROID__
    // A close button a finger can find. On a desktop the map closes with M or
    // Escape, and hosted in FrameXML's frame it has no title bar and so no X
    // of its own - which on a phone, with no keyboard, was a map that opened
    // and could not be closed. Back closes it as Escape does; this is the
    // button for whoever does not think to try that.
    if (wm->isOpen() || wm->isTaxiMapOpen()) {
        const ImGuiIO& io = ImGui::GetIO();
        const float side = ImGui::GetFrameHeight() * 1.8f;
        const float margin = side * 0.4f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - side - margin, margin));
        // In front of the map, which takes focus whenever it is touched.
        ImGui::SetNextWindowFocus();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, side * 0.5f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##WorldMapClose", nullptr, flags)) {
            if (ImGui::Button("X", ImVec2(side, side))) {
                if (wm->isTaxiMapOpen()) {
                    gameHandler.closeTaxi();
                } else if (frameXmlDrivesMap) {
                    // FrameXML's frame is what holds the map open; hiding it
                    // takes the map's rect away and the map with it.
                    if (auto* addons = app.getAddonManager()) {
                        if (auto* engine = addons->getLuaEngine()) {
                            engine->executeString(
                                "if WorldMapFrame and WorldMapFrame:IsShown() then "
                                "HideUIPanel(WorldMapFrame) end");
                        }
                    }
                } else {
                    wm->close();
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
#endif

    // Sync showWorldMap_ if the map closed itself (e.g. ESC key inside the overlay).
    // Only where that flag is what opened it: under FrameXML the frame's own
    // visibility is the state, and clearing this would say nothing.
    if (!frameXmlDrivesMap && !wm->isOpen()) showWorldMap_ = false;
}

// ============================================================
// Action Bar
// ============================================================

VkDescriptorSet GameScreen::getSpellIcon(uint32_t spellId, pipeline::AssetManager* am) {
    if (spellId == 0 || !am) return VK_NULL_HANDLE;

    // Check cache first
    auto cit = spellIconCache_.find(spellId);
    if (cit != spellIconCache_.end()) return cit->second;

    // Lazy-load SpellIcon.dbc and Spell.dbc icon IDs
    if (!spellIconDbLoaded_) {
        spellIconDbLoaded_ = true;

        // Load SpellIcon.dbc: field 0 = ID, field 1 = icon path
        pipeline::loadSpellIconPaths(am, spellIconPaths_);

        // Load Spell.dbc: SpellIconID field
        auto spellDbc = am->loadDBC("Spell.dbc");
        const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
        if (spellDbc && spellDbc->isLoaded()) {
            uint32_t fieldCount = spellDbc->getFieldCount();
            // Helper to load icons for a given field layout
            auto tryLoadIcons = [&](uint32_t idField, uint32_t iconField) {
                spellIconIds_.clear();
                if (iconField >= fieldCount) return;
                for (uint32_t i = 0; i < spellDbc->getRecordCount(); i++) {
                    uint32_t id = spellDbc->getUInt32(i, idField);
                    uint32_t iconId = spellDbc->getUInt32(i, iconField);
                    if (id > 0 && iconId > 0) {
                        spellIconIds_[id] = iconId;
                    }
                }
            };

            // Use the active expansion layout when its fields are present in
            // the loaded DBC. TBC/WotLK/Classic place IconID in different
            // columns, so reading the WotLK default for every client leaves
            // action bars and spell UI without icons.
            uint32_t iconField = 133; // WotLK default
            uint32_t idField = 0;
            if (spellL) {
                try {
                    uint32_t layoutId = (*spellL)["ID"];
                    uint32_t layoutIcon = (*spellL)["IconID"];
                    if (layoutId < fieldCount && layoutIcon < fieldCount) {
                        iconField = layoutIcon;
                        idField = layoutId;
                    }
                } catch (...) {}
            }
            tryLoadIcons(idField, iconField);
        }
    }

    // Rate-limit GPU uploads per frame to prevent stalls when many icons are uncached
    // (e.g., first login, after loading screen, or many new auras appearing at once).
    if (!claimUiTextureUpload()) return VK_NULL_HANDLE;  // defer - do NOT cache null here

    // Look up spellId -> SpellIconID -> icon path
    auto iit = spellIconIds_.find(spellId);
    if (iit == spellIconIds_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    auto pit = spellIconPaths_.find(iit->second);
    if (pit == spellIconPaths_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Path from DBC has no extension - append .blp
    std::string iconPath = pit->second + ".blp";
    // Cached either way, failures included: the HUD asks for this every frame
    // an aura is up, so a missing icon must not be retried each time.
    VkDescriptorSet ds =
        uploadUiTextureFromBlp(am, iconPath, services_.window);
    spellIconCache_[spellId] = ds;
    return ds;
}

// ============================================================
// Cooldown Tracker - floating panel showing all active spell CDs
// ============================================================

// ============================================================
// Quest Objective Tracker (right-side HUD)
// ============================================================

// ============================================================
// Nameplates - world-space health bars projected to screen
// ============================================================

// ============================================================
// Durability Warning (equipment damage indicator)
// ============================================================

// The settings panel keeps brightness as 0-100 with 50 neutral, and the post
// process pipeline wants that over 50 - so the number the video options call
// gamma is exactly what the pipeline is already given, and this converts
// between the two rather than introducing a third scale.
float GameScreen::getGamma() const {
    return static_cast<float>(settingsPanel_.pendingBrightness) / 50.0f;
}

void GameScreen::setGamma(float gamma) {
    // WoW's own slider runs 0.3 to 2.8; clamped to what the 0-100 setting can
    // hold so a value from outside cannot push the slider off its own track.
    const float clamped = std::clamp(gamma, 0.0f, 2.0f);
    const int stored = static_cast<int>(std::lround(clamped * 50.0f));
    // Saved here, because nothing else was going to.
    //
    // Every other route into these settings goes through the settings window,
    // which writes the file when it is done. The video options' Gamma slider
    // reaches this directly from Lua instead, so the value applied, looked
    // right for the rest of the session, and was gone the next time the client
    // started - the file had never been written.
    //
    // Only when the stored number actually moves. The slider reports every
    // frame it is dragged, and the setting is a whole number out of a hundred,
    // so this is a handful of writes across a drag rather than one per frame.
    const bool changed = (stored != settingsPanel_.pendingBrightness);
    settingsPanel_.pendingBrightness = stored;
    if (auto* renderer = services_.renderer) {
        renderer->getPostProcessPipeline()->setBrightness(clamped);
    }
    if (changed) saveSettings();
}

namespace {

/// ~/.wowee/<folder>/WoWee_YYYYMMDD_HHMMSS.<extension>, the name a screenshot
/// or a recording is saved under.
std::string capturePath(const char* folder, const char* extension) {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.wowee/" + folder;

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    tm = core::localTime(tt);

    char filename[128];
    std::snprintf(filename, sizeof(filename),
                  "WoWee_%04d%02d%02d_%02d%02d%02d.%s",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, extension);
    return dir + "/" + filename;
}

}  // namespace

void GameScreen::startRecording() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler || renderer->isRecording()) return;
    const std::string path = capturePath("recordings", "mp4");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::string error;
    if (renderer->startRecording(path, error)) {
        recordingPath_ = path;
        services_.gameHandler->addSystemChatMessage(
            "Recording to " + path + ". Type /record again to stop.");
    } else {
        services_.gameHandler->addSystemChatMessage("Could not start recording: " + error + ".");
    }
}

void GameScreen::stopRecording() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler || !renderer->isRecording()) return;
    const auto stats = renderer->stopRecording();
    const int seconds = static_cast<int>(stats.seconds + 0.5);
    char length[32];
    std::snprintf(length, sizeof(length), "%d:%02d", seconds / 60, seconds % 60);
    std::string message = "Recording saved: " + recordingPath_ + " (" + length;
    if (!stats.hasAudio) message += ", no sound";
    if (stats.framesDropped > 0) {
        message += ", " + std::to_string(stats.framesDropped) + " frames dropped";
    }
    services_.gameHandler->addSystemChatMessage(message + ").");
}

void GameScreen::toggleRecording() {
    auto* renderer = services_.renderer;
    if (!renderer) return;
    if (renderer->isRecording()) {
        stopRecording();
    } else {
        startRecording();
    }
}

void GameScreen::reportRecordingFailure() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler) return;
    const std::string failure = renderer->takeRecordingFailure();
    if (failure.empty()) return;
    services_.gameHandler->addSystemChatMessage(
        "Recording stopped: " + failure + ". What was recorded is saved in " + recordingPath_ + ".");
}

void GameScreen::takeScreenshot() {
    auto* renderer = services_.renderer;
    if (!renderer) return;

    const std::string path = capturePath("screenshots", "png");

    if (renderer->captureScreenshot(path)) {
        game::MessageChatData sysMsg;
        sysMsg.type = game::ChatType::SYSTEM;
        sysMsg.language = game::ChatLanguage::UNIVERSAL;
        sysMsg.message = "Screenshot saved: " + path;
        services_.gameHandler->addLocalChatMessage(sysMsg);
    }
}

}} // namespace wowee::ui
