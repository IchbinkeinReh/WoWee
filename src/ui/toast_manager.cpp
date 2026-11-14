#include "game/reputation_standing.hpp"
#include "ui/toast_manager.hpp"
#include "game/game_handler.hpp"
#include "core/application.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "ui/framexml_takeover.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace wowee { namespace ui {

// ---------------------------------------------------------------------------
// Render late toasts (after escape menu / settings)
// ---------------------------------------------------------------------------
void ToastManager::renderLateToasts(game::GameHandler& gameHandler) {
    // The zone text only. The "You have reached level N!" banner with its
    // stat gains, and the resurrection flash with its "You have been
    // resurrected" panel, are gone: 3.3.5a draws neither. A level gained is
    // chatframe.lua's PLAYER_LEVEL_UP branch (LEVEL_UP, LEVEL_UP_HEALTH_MANA,
    // LEVEL_UP_STAT...) from the event SMSG_LEVELUP_INFO fires, and coming
    // back to life ends the ghost look and nothing else.
    renderZoneText(gameHandler);
}

void ToastManager::playLevelUp() {
    // The sound alone. The character does not cheer: SMSG_LEVELUP_INFO
    // (0x006e2e90 case 0x1d4) opens tutorials and fires PLAYER_LEVEL_UP, and
    // a level field that changes plays "HARDCODED Unit Level Up" on the unit
    // (0x00730050 -> 0x0072e3a0 -> 0x006f9260) - the LevelUp.m2 effect, no
    // emote and no animation of the character's own.
    auto* ac = services_.audioCoordinator;
    if (ac) {
        if (auto* sfx = ac->getUiSoundManager()) {
            sfx->playLevelUp();
        }
    }
}

void ToastManager::playAchievementEarned() {
    // alertframes.lua registers ACHIEVEMENT_EARNED and raises the badge, so
    // there is nothing of ours left to put on screen. The sound stays:
    // FrameXML's alert frame plays one only for LFG rewards, and an
    // achievement earned in silence is worse than one announced twice.
    auto* ac = services_.audioCoordinator;
    if (ac) {
        if (auto* sfx = ac->getUiSoundManager()) {
            sfx->playAchievementAlert();
        }
    }
}

// ---------------------------------------------------------------------------

void ToastManager::renderZoneText(game::GameHandler& gameHandler) {
    // Poll worldStateZoneId for server-driven zone changes (fires on every zone crossing,
    // including sub-zones like Ironforge within Dun Morogh).
    uint32_t wsZoneId = gameHandler.getWorldStateZoneId();
    if (wsZoneId != 0 && wsZoneId != lastKnownWorldStateZoneId_) {
        lastKnownWorldStateZoneId_ = wsZoneId;
        std::string wsName = gameHandler.getWhoAreaName(wsZoneId);
        if (!wsName.empty()) {
            zoneTextName_  = wsName;
            zoneTextTimer_ = ZONE_TEXT_DURATION;
        }
    }

    // Also poll the renderer for zone name changes (covers map-level transitions
    // where worldStateZoneId may not change immediately).
    auto* appRenderer = services_.renderer;
    if (appRenderer) {
        const std::string& zoneName = appRenderer->getCurrentZoneName();
        if (!zoneName.empty() && zoneName != lastKnownZoneName_) {
            lastKnownZoneName_ = zoneName;
            // Only override if the worldState hasn't already queued this zone
            if (zoneTextName_ != zoneName) {
                zoneTextName_  = zoneName;
                zoneTextTimer_ = ZONE_TEXT_DURATION;
            }
        }
    }

    if (zoneTextTimer_ <= 0.0f || zoneTextName_.empty()) return;

    float dt = ImGui::GetIO().DeltaTime;
    zoneTextTimer_ -= dt;
    if (zoneTextTimer_ < 0.0f) zoneTextTimer_ = 0.0f;

    // Drawn only if FrameXML is not, but tracked either way: the zone toasts
    // above are suppressed while this timer is running, so a plain early
    // return at the top of this function would swap one duplicate for
    // another - the banner would go and "Entering: Stormwind City" would come
    // back beside FrameXML's.
    if (frameXmlOwns(UiElement::ZoneText)) return;

    auto* window = services_.window;
    float screenW = window ? static_cast<float>(window->getWidth())  : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) :  720.0f;

    // Fade: ramp up in first 0.5 s, hold, fade out in last 1.0 s
    float alpha;
    if (zoneTextTimer_ > ZONE_TEXT_DURATION - 0.5f)
        alpha = 1.0f - (zoneTextTimer_ - (ZONE_TEXT_DURATION - 0.5f)) / 0.5f;
    else if (zoneTextTimer_ < 1.0f)
        alpha = zoneTextTimer_;
    else
        alpha = 1.0f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    ImFont* font = ImGui::GetFont();

    // "Entering:" header
    const char* header = "Entering:";
    float headerSize = 16.0f;
    float nameSize   = 26.0f;

    ImVec2 headerDim = font->CalcTextSizeA(headerSize, FLT_MAX, 0.0f, header);
    ImVec2 nameDim   = font->CalcTextSizeA(nameSize,   FLT_MAX, 0.0f, zoneTextName_.c_str());

    float centreY = screenH * 0.30f;  // upper third, like WoW
    float headerX = (screenW - headerDim.x) * 0.5f;
    float nameX   = (screenW - nameDim.x)   * 0.5f;
    float headerY = centreY;
    float nameY   = centreY + headerDim.y + 4.0f;

    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // "Entering:" in gold
    draw->AddText(font, headerSize, ImVec2(headerX + 1, headerY + 1),
                  IM_COL32(0, 0, 0, static_cast<int>(alpha * 160)), header);
    draw->AddText(font, headerSize, ImVec2(headerX, headerY),
                  IM_COL32(255, 215, 0, static_cast<int>(alpha * 255)), header);

    // Zone name in white
    draw->AddText(font, nameSize, ImVec2(nameX + 1, nameY + 1),
                  IM_COL32(0, 0, 0, static_cast<int>(alpha * 160)), zoneTextName_.c_str());
    draw->AddText(font, nameSize, ImVec2(nameX, nameY),
                  IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)), zoneTextName_.c_str());
}

} } // namespace wowee::ui
