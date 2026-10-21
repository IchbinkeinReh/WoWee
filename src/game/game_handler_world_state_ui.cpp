// game_handler_world_state_ui.cpp - The world state frame's lines as the
// client chooses them from WorldStateUI.dbc (WorldStateUI.cpp 0x00548570-
// 0x00548d40), and the phases and server time that bear on them.
#include "game/game_handler.hpp"
#include "network/packet.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

#include <ctime>

namespace wowee::game {

void GameHandler::registerWorldStateUiOpcodes() {
    // 0x00526530 case 0x47c: the player's phases (0x00513960), the lines
    // chosen again and UPDATE_WORLD_STATES (event 0x1d0) when they moved.
    dispatchTable_[Opcode::SMSG_SET_PHASE_SHIFT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        world_state_ui::Place place = worldStateUiPlace_;
        place.phaseMask = packet.readUInt32();
        if (setWorldStateUiPlace(place)) fireAddonEvent("UPDATE_WORLD_STATES", {});
    };
}

void GameHandler::loadWorldStateUi() {
    if (worldStateUiLoaded_) return;
    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return;
    worldStateUiLoaded_ = true;
    const auto* layout =
        pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("WorldStateUI") : nullptr;
    auto dbc = layout ? am->loadDBC("WorldStateUI.dbc") : nullptr;
    if (!dbc || !dbc->isLoaded()) return;
    const auto& f = *layout;
    if (dbc->getFieldCount() <= f["ExtendedUIStateVariable"] + 2) return;
    worldStateUiRows_.reserve(dbc->getRecordCount());
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        world_state_ui::Row r;
        r.id = dbc->getUInt32(i, f["ID"]);
        r.mapId = static_cast<int32_t>(dbc->getUInt32(i, f["MapID"]));
        r.areaId = dbc->getUInt32(i, f["AreaID"]);
        r.phaseShift = dbc->getUInt32(i, f["PhaseShift"]);
        r.icon = dbc->getString(i, f["Icon"]);
        r.text = dbc->getString(i, f["String"]);
        r.tooltip = dbc->getString(i, f["Tooltip"]);
        r.stateVariable = dbc->getUInt32(i, f["StateVariable"]);
        r.type = dbc->getUInt32(i, f["Type"]);
        r.dynamicIcon = dbc->getString(i, f["DynamicIcon"]);
        r.dynamicTooltip = dbc->getString(i, f["DynamicTooltip"]);
        r.extendedUI = dbc->getString(i, f["ExtendedUI"]);
        for (uint32_t k = 0; k < 3; ++k) r.extendedUIState[k] = dbc->getUInt32(i, f["ExtendedUIStateVariable"] + k);
        worldStateUiRows_.push_back(std::move(r));
    }
    worldStateUiShown_ = world_state_ui::shownRows(worldStateUiRows_, worldStateUiPlace_);
}

bool GameHandler::setWorldStateUiPlace(const world_state_ui::Place& place) {
    if (place == worldStateUiPlace_) return false;
    worldStateUiPlace_ = place;
    loadWorldStateUi();
    worldStateUiShown_ = world_state_ui::shownRows(worldStateUiRows_, worldStateUiPlace_);
    return true;
}

std::optional<std::vector<const world_state_ui::Row*>> GameHandler::worldStateUiLines() {
    loadWorldStateUi();
    if (worldStateUiRows_.empty()) return std::nullopt;
    std::vector<const world_state_ui::Row*> lines;
    lines.reserve(worldStateUiShown_.size());
    for (size_t i : worldStateUiShown_) lines.push_back(&worldStateUiRows_[i]);
    return lines;
}

int64_t GameHandler::worldStateNow() const {
    return static_cast<int64_t>(std::time(nullptr)) + worldStateTimeOffset_;
}

}  // namespace wowee::game
