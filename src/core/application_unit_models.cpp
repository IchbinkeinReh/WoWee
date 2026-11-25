// What a unit looks like (ui/unit_look.hpp), and the model frames that show a
// unit or a creature: CharacterModelBase.cpp's PlayerModel, DressUpModel and
// TabardModel, whoever made the frame.

#include "core/application.hpp"

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/entity_spawner.hpp"
#include "game/character.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/figure_camera.hpp"
#include "rendering/model_frame_view.hpp"
#include "rendering/renderer.hpp"
#include "ui/unit_portrait.hpp"
#include "ui/widget_tree.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace wowee::core {

namespace {

std::vector<game::EquipmentItem> visiblyWorn(const game::GameHandler& gameHandler, uint64_t guid) {
    std::vector<game::EquipmentItem> worn;
    std::array<uint32_t, 19> displayIds{};
    std::array<uint8_t, 19> invTypes{};
    if (!gameHandler.getOtherPlayerEquipment(guid, displayIds, invTypes)) return worn;
    for (size_t slot = 0; slot < displayIds.size(); ++slot) {
        if (displayIds[slot] == 0) continue;
        worn.push_back({.displayModel = displayIds[slot], .inventoryType = invTypes[slot], .enchantment = 0u});
    }
    return worn;
}

}  // namespace

const ui::UnitLook& Application::displayLook(uint32_t displayId) {
    static const ui::UnitLook kNone;
    if (displayId == 0 || !entitySpawner_ || !entitySpawner_->areCreatureLookupsBuilt()) return kNone;
    if (const auto it = displayLooks_.find(displayId); it != displayLooks_.end()) return it->second;
    constexpr size_t kKept = 256;
    if (displayLooks_.size() >= kKept) displayLooks_.clear();
    ui::UnitLook look;
    // A humanoid NPC - a guard, a questgiver, an innkeeper - is drawn the way
    // a character is, from a race and the same skin, face and hair choices a
    // player has. CreatureDisplayInfo leaves the skin fields empty for these,
    // so loading one as a creature gives a shape with nothing on it.
    uint8_t race = 0, sex = 0, facial = 0;
    uint32_t bytes = 0;
    if (entitySpawner_->getHumanoidAppearance(displayId, race, sex, bytes, facial)) {
        look.character = true;
        look.race = race;
        look.gender = sex;
        look.femaleModel = sex == 1;
        look.appearance = bytes;
        look.facialFeatures = facial;
        for (const auto& [display, invType] : entitySpawner_->getHumanoidEquipment(displayId))
            look.worn.push_back({.displayModel = display, .inventoryType = invType, .enchantment = 0u});
        // The bake, where there is one - nearly always. It is the whole
        // appearance already composited, armour included; the equipment above
        // still decides the geosets, and the bake only paints what they draw.
        look.bakedSkin = entitySpawner_->getHumanoidBakePath(displayId);
    } else {
        look.model = entitySpawner_->getModelPathForDisplayId(displayId);
        if (!look.model.empty()) look.skins = entitySpawner_->getCreatureSkinPaths(displayId, look.model);
    }
    if (!look.any()) return kNone;
    return displayLooks_.emplace(displayId, std::move(look)).first->second;
}

ui::UnitLook Application::unitLook(uint64_t guid) {
    ui::UnitLook look;
    if (guid == 0 || !gameHandler) return look;
    const bool self = guid == gameHandler->getPlayerGuid();
    const auto entity = gameHandler->getEntityManager().getEntity(guid);
    const bool unit = entity && entity->isUnit();
    const auto field = [&](game::UF f) {
        const uint16_t i = game::fieldIndex(f);
        return unit && i != 0xFFFF ? entity->getField(i) : 0u;
    };
    const uint32_t display = field(game::UF::UNIT_FIELD_DISPLAYID);
    const uint32_t native = field(game::UF::UNIT_FIELD_NATIVEDISPLAYID);
    const bool player = self || (entity && entity->getType() == game::ObjectType::PLAYER);
    if (ui::drawnAsCharacter(player, display, native)) {
        if (self) {
            // The character list is where the player's own appearance is
            // described. What it wears is the inventory's, not the list's,
            // which is never updated: that stands in until the item queries
            // have come back, which is a moment at most.
            for (const auto& c : gameHandler->getCharacters()) {
                if (c.guid != guid) continue;
                look.character = true;
                look.race = static_cast<uint8_t>(c.race);
                look.gender = static_cast<uint8_t>(c.gender);
                look.femaleModel = c.useFemaleModel;
                look.classId = static_cast<uint8_t>(c.characterClass);
                look.appearance = c.appearanceBytes;
                look.facialFeatures = c.facialFeatures;
                look.worn = visiblyWorn(*gameHandler, guid);
                if (look.worn.empty()) look.worn = c.equipment;
                // The guild tabard's design (0x006db510): the guild's, once
                // known, or the tabard designer's while it is open (0x004ec1c0).
                look.emblem = gameHandler->getPlayerTabardEmblem();
                look.tabardDesigner = gameHandler->getTabardVendorGuid() != 0;
                return look;
            }
        } else if (gameHandler->getPlayerAppearance(guid, look.race, look.gender, look.appearance,
                                                    look.facialFeatures)) {
            look.character = true;
            look.femaleModel = look.gender == 1;
            look.classId = gameHandler->lookupPlayerClass(guid);
            look.worn = visiblyWorn(*gameHandler, guid);
            look.emblem = gameHandler->lookupGuildEmblem(gameHandler->getEntityGuildId(guid));
            return look;
        }
    }
    if (!unit || !entitySpawner_) return {};
    return displayLook(player ? display : entitySpawner_->mirrorDisplayId(guid, display));
}

void Application::drawUnitModels(ui::WidgetTree& widgets, float screenW, float screenH, float deltaTime) {
    ++unitFigureFrame_;
    std::vector<uint32_t> updated;
    const float uiScale = widgets.uiScale();
    if (screenW > 0.0f && screenH > 0.0f && uiScale > 0.0f) {
        const float ddcH = rendering::model_frame::ddcHeight(screenW / screenH);
        for (const uint32_t id : widgets.unitModelFrames()) {
            ui::Widget* frame = widgets.get(id);
            if (!frame) continue;
            if (!frame->visible || frame->rectW <= 0.0f || frame->rectH <= 0.0f) {
                frame->externalTexture = 0;
                continue;
            }
            UnitFigure& figure = unitFigures_[id];
            figure.shown = unitFigureFrame_;
            if (frame->modelSource == ui::Widget::ModelSource::Unit) {
                // A unit out of sight leaves what was there (0x005977e0).
                if (auto look = unitLook(frame->modelUnit); look.any()) figure.look = std::move(look);
            } else {
                // The creature may not have been known as it was named.
                figure.look = displayLook(frame->shownDisplay([this](uint32_t entry) {
                    return gameHandler->getCreatureDisplayIdForEntry(entry);
                }));
            }
            ui::UnitLook look = figure.look;
            if (!look.any()) {
                frame->externalTexture = 0;
                continue;
            }
            if (look.character && frame->objectType == "DressUpModel")
                look.worn = ui::wornWithTriedOn(std::move(look.worn), frame->undressed, frame->tryOnItems,
                                                frame->modelUnit == gameHandler->getPlayerGuid() &&
                                                    !frame->tryOnItems.empty() && gameHandler->playerCanDualWield());
            frame->modelShown = look.character
                                    ? game::getPlayerModelPath(static_cast<game::Race>(look.race),
                                                               static_cast<game::Gender>(look.gender), look.femaleModel)
                                    : look.model;
            if (!figure.view) figure.view = std::make_unique<ui::UnitPortrait>();
            // The frame's own shape: its camera's view is of that.
            const float w = frame->rectW * uiScale, h = frame->rectH * uiScale;
            constexpr float kLargest = 1024.0f;
            const float fit = std::min(1.0f, kLargest / std::max(w, h));
            const auto pixels = [fit](float v) { return std::max(16, static_cast<int>(std::lround(v * fit))); };
            figure.view->fitTo(pixels(w), pixels(h), renderer.get());
            figure.view->setFraming(ui::UnitPortrait::Framing::Figure);
            const auto vec = [](const float (&v)[3]) { return glm::vec3(v[0], v[1], v[2]); };
            figure.view->setFigure(frame->modelCamera,
                                   rendering::figure_camera::pose(vec(frame->modelPosition), frame->modelScale,
                                                                  frame->modelFacing, vec(frame->modelCameAt),
                                                                  frame->modelCameScale, ddcH));
            const bool drawn = figure.view->show(look, assetManager.get(), renderer.get(), deltaTime);
            figure.view->setTurning(frame->modelTurn, frame->modelTurnSerial);
            figure.view->setSequence(frame->modelSequence, frame->modelSequenceSerial);
            frame->externalTexture = drawn ? figure.view->textureId() : 0;
            if (drawn) updated.push_back(id);
        }
    }
    // OnUpdateModel (0x0095fba0), once all are placed: a script may make frames.
    if (auto* engine = addonManager_ ? addonManager_->getLuaEngine() : nullptr)
        for (const uint32_t id : updated) engine->runFrameScript(id, "OnUpdateModel");
    // A frame that shows nothing of the sort any more has no model
    // (0x00597210); its view is kept for when it does again, up to a few.
    constexpr size_t kKept = 12;
    for (auto& [id, figure] : unitFigures_)
        if (figure.shown != unitFigureFrame_) figure.look = {};
    while (unitFigures_.size() > kKept) {
        auto oldest = unitFigures_.end();
        for (auto it = unitFigures_.begin(); it != unitFigures_.end(); ++it) {
            if (it->second.shown == unitFigureFrame_) continue;
            if (oldest == unitFigures_.end() || it->second.shown < oldest->second.shown) oldest = it;
        }
        if (oldest == unitFigures_.end()) break;
        if (oldest->second.view) oldest->second.view->shutdown(renderer.get());
        unitFigures_.erase(oldest);
    }
}

}  // namespace wowee::core
