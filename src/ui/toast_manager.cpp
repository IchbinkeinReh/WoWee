#include "game/reputation_standing.hpp"
#include "ui/toast_manager.hpp"
#include "game/game_handler.hpp"
#include "core/application.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "ui/framexml_takeover.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace wowee { namespace ui {

// ---------------------------------------------------------------------------
// Setup toast callbacks on GameHandler (idempotent - safe to call every frame)
// ---------------------------------------------------------------------------
void ToastManager::setupCallbacks(game::GameHandler& gameHandler) {
    // Only the resurrection flash. The reputation, honor, loot, whisper,
    // quest progress and complete, discovery, zone, area trigger and
    // nearby level-up toasts are gone: the client has none of them, and
    // each of those events already has its chat or info line (FACTION_
    // STANDING_*, COMBATLOG_HONORGAIN, LOOT_ITEM_*, ERR_QUEST_ADD_*,
    // ERR_ZONE_EXPLORED, ZoneTextFrame, UI_INFO_MESSAGE).
    if (!ghostStateCallbackSet_) {
        gameHandler.setGhostStateCallback([this](bool isGhost) {
            if (!isGhost) {
                resurrectFlashTimer_ = kResurrectFlashDuration;
            }
        });
        ghostStateCallbackSet_ = true;
    }
}

// ---------------------------------------------------------------------------
// Render late toasts (after escape menu / settings)
// ---------------------------------------------------------------------------
void ToastManager::renderLateToasts(game::GameHandler& gameHandler) {
    renderDingEffect();
    renderResurrectFlash();
    renderZoneText(gameHandler);
}

// ============================================================
// Level-Up Ding Animation
// ============================================================

void ToastManager::triggerDing(uint32_t newLevel, uint32_t hpDelta, uint32_t manaDelta,
                              uint32_t str, uint32_t agi, uint32_t sta,
                              uint32_t intel, uint32_t spi) {
    // Set golden burst overlay state (consumed by GameScreen)
    levelUpFlashAlpha = 1.0f;
    levelUpDisplayLevel = newLevel;

    dingTimer_     = DING_DURATION;
    dingLevel_     = newLevel;
    dingHpDelta_   = hpDelta;
    dingManaDelta_ = manaDelta;
    dingStats_[0]  = str;
    dingStats_[1]  = agi;
    dingStats_[2]  = sta;
    dingStats_[3]  = intel;
    dingStats_[4]  = spi;

    auto* ac = services_.audioCoordinator;
    if (ac) {
        if (auto* sfx = ac->getUiSoundManager()) {
            sfx->playLevelUp();
        }
    }
    if (auto* renderer = services_.renderer) {
        if (auto* ac = renderer->getAnimationController()) ac->playEmote("cheer");
    }
}

void ToastManager::renderDingEffect() {
    if (dingTimer_ <= 0.0f) return;

    float dt = ImGui::GetIO().DeltaTime;
    dingTimer_ -= dt;
    if (dingTimer_ < 0.0f) dingTimer_ = 0.0f;

    // Show "You have reached level X!" for the first 2.5s, fade out over last 0.5s.
    // The 3D visual effect is handled by Renderer::triggerLevelUpEffect (LevelUp.m2).
    constexpr float kFadeTime = 0.5f;
    float alpha = dingTimer_ < kFadeTime ? (dingTimer_ / kFadeTime) : 1.0f;
    if (alpha <= 0.0f) return;

    ImGuiIO& io = ImGui::GetIO();
    float cx = io.DisplaySize.x * 0.5f;
    float cy = io.DisplaySize.y * 0.38f;  // Upper-center, like WoW

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float baseSize = ImGui::GetFontSize();
    float fontSize = baseSize * 1.8f;

    char buf[64];
    snprintf(buf, sizeof(buf), "You have reached level %u!", dingLevel_);

    ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf);
    float tx = cx - sz.x * 0.5f;
    float ty = cy - sz.y * 0.5f;

    // Slight black outline for readability
    draw->AddText(font, fontSize, ImVec2(tx + 2, ty + 2),
                  IM_COL32(0, 0, 0, static_cast<int>(alpha * 180)), buf);
    // Gold text
    draw->AddText(font, fontSize, ImVec2(tx, ty),
                  IM_COL32(255, 210, 0, static_cast<int>(alpha * 255)), buf);

    // Stat gains below the main text (shown only if server sent deltas)
    bool hasStatGains = (dingHpDelta_ > 0 || dingManaDelta_ > 0 ||
                         dingStats_[0] || dingStats_[1] || dingStats_[2] ||
                         dingStats_[3] || dingStats_[4]);
    if (hasStatGains) {
        float smallSize = baseSize * 0.95f;
        float yOff = ty + sz.y + 6.0f;

        // Build stat delta string: "+150 HP  +80 Mana  +2 Str  +2 Agi ..."
        static constexpr const char* kStatLabels[] = { "Str", "Agi", "Sta", "Int", "Spi" };
        char statBuf[128];
        int written = 0;
        if (dingHpDelta_ > 0)
            written += snprintf(statBuf + written, sizeof(statBuf) - written,
                                "+%u HP  ", dingHpDelta_);
        if (dingManaDelta_ > 0)
            written += snprintf(statBuf + written, sizeof(statBuf) - written,
                                "+%u Mana  ", dingManaDelta_);
        for (int i = 0; i < 5 && written < static_cast<int>(sizeof(statBuf)) - 1; ++i) {
            if (dingStats_[i] > 0)
                written += snprintf(statBuf + written, sizeof(statBuf) - written,
                                    "+%u %s  ", dingStats_[i], kStatLabels[i]);
        }
        // Trim trailing spaces
        while (written > 0 && statBuf[written - 1] == ' ') --written;
        statBuf[written] = '\0';

        if (written > 0) {
            ImVec2 ssz = font->CalcTextSizeA(smallSize, FLT_MAX, 0.0f, statBuf);
            float stx = cx - ssz.x * 0.5f;
            draw->AddText(font, smallSize, ImVec2(stx + 1, yOff + 1),
                          IM_COL32(0, 0, 0, static_cast<int>(alpha * 160)), statBuf);
            draw->AddText(font, smallSize, ImVec2(stx, yOff),
                          IM_COL32(100, 220, 100, static_cast<int>(alpha * 230)), statBuf);
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
// Resurrection flash - brief screen brightening + "You have been resurrected!"
// banner when the player transitions from ghost back to alive.
// ---------------------------------------------------------------------------

void ToastManager::renderResurrectFlash() {
    if (resurrectFlashTimer_ <= 0.0f) return;

    float dt = ImGui::GetIO().DeltaTime;
    resurrectFlashTimer_ -= dt;
    if (resurrectFlashTimer_ <= 0.0f) {
        resurrectFlashTimer_ = 0.0f;
        return;
    }

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float screenW = displaySize.x > 0.0f ? displaySize.x : 1280.0f;
    float screenH = displaySize.y > 0.0f ? displaySize.y : 720.0f;

    // Normalised age in [0, 1] (0 = just fired, 1 = fully elapsed)
    float t = 1.0f - resurrectFlashTimer_ / kResurrectFlashDuration;

    // Alpha envelope: fast fade-in (first 0.15s), hold, then fade-out (last 0.8s)
    float alpha;
    const float fadeIn  = 0.15f / kResurrectFlashDuration;   // ~5% of lifetime
    const float fadeOut = 0.8f  / kResurrectFlashDuration;   // ~27% of lifetime
    if (t < fadeIn)
        alpha = t / fadeIn;
    else if (t < 1.0f - fadeOut)
        alpha = 1.0f;
    else
        alpha = (1.0f - t) / fadeOut;
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // Soft golden/white vignette - brightening instead of darkening
    uint8_t vigA = static_cast<uint8_t>(50 * alpha);
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, screenH),
                      IM_COL32(200, 230, 255, vigA));

    // Centered banner panel
    constexpr float PANEL_W = 360.0f;
    constexpr float PANEL_H = 52.0f;
    float px = (screenW - PANEL_W) * 0.5f;
    float py = screenH * 0.34f;

    uint8_t bgA     = static_cast<uint8_t>(210 * alpha);
    uint8_t borderA = static_cast<uint8_t>(255 * alpha);
    uint8_t textA   = static_cast<uint8_t>(255 * alpha);

    // Background: deep blue-black
    bg->AddRectFilled(ImVec2(px, py), ImVec2(px + PANEL_W, py + PANEL_H),
                      IM_COL32(10, 18, 40, bgA), 8.0f);

    // Border glow: bright holy gold
    bg->AddRect(ImVec2(px, py), ImVec2(px + PANEL_W, py + PANEL_H),
                IM_COL32(200, 230, 100, borderA), 8.0f, 0, 2.0f);
    // Inner halo line
    bg->AddRect(ImVec2(px + 3.0f, py + 3.0f), ImVec2(px + PANEL_W - 3.0f, py + PANEL_H - 3.0f),
                IM_COL32(255, 255, 180, static_cast<uint8_t>(80 * alpha)), 6.0f, 0, 1.0f);

    // Resurrection message centered without decorative punctuation.
    const char* banner = "You have been resurrected";
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();
    ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, banner);
    float tx = px + (PANEL_W - textSz.x) * 0.5f;
    float ty = py + (PANEL_H - textSz.y) * 0.5f;

    // Drop shadow
    bg->AddText(font, fontSize, ImVec2(tx + 1.0f, ty + 1.0f),
                IM_COL32(0, 0, 0, static_cast<uint8_t>(180 * alpha)), banner);
    // Main text in warm gold
    bg->AddText(font, fontSize, ImVec2(tx, ty),
                IM_COL32(255, 240, 120, textA), banner);
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
