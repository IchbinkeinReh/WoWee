#pragma once

#include "ui/ui_services.hpp"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace game { class GameHandler; }
namespace ui {

/**
 * Toast / notification overlay manager
 *
 * Owns all toast state, callbacks, and rendering:
 *   level-up ding, achievement sound, resurrect flash and zone text. The
 *   client has no reputation, honor, loot, whisper, quest or discovery
 *   toasts; those events are chat and info lines.
 */
class ToastManager {
public:
    ToastManager() = default;

    /// Register toast-related callbacks on GameHandler (idempotent - safe every frame)
    void setupCallbacks(game::GameHandler& gameHandler);

    /// Render the ding, the resurrection flash and the zone text - called after
    /// the escape menu
    void renderLateToasts(game::GameHandler& gameHandler);

    /// Fire level-up ding animation + sound
    void triggerDing(uint32_t newLevel, uint32_t hpDelta = 0, uint32_t manaDelta = 0,
                     uint32_t str = 0, uint32_t agi = 0, uint32_t sta = 0,
                     uint32_t intel = 0, uint32_t spi = 0);

    /// Fire achievement earned toast + sound
    /// The sound an achievement makes. The badge is FrameXML's.
    void playAchievementEarned();

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

    // --- public state consumed by GameScreen for the golden burst overlay ---
    float levelUpFlashAlpha = 0.0f;
    uint32_t levelUpDisplayLevel = 0;

private:
    // Injected UI services
    UIServices services_;

    // ---- Ding effect (own level-up) ----
    static constexpr float DING_DURATION = 4.0f;
    float dingTimer_ = 0.0f;
    uint32_t dingLevel_ = 0;
    uint32_t dingHpDelta_   = 0;
    uint32_t dingManaDelta_ = 0;
    uint32_t dingStats_[5]  = {};
    void renderDingEffect();

    // ---- Achievement toast ----
    static constexpr float ACHIEVEMENT_TOAST_DURATION = 5.0f;

    // ---- Resurrection flash ----
    float resurrectFlashTimer_ = 0.0f;
    static constexpr float kResurrectFlashDuration = 3.0f;
    bool ghostStateCallbackSet_ = false;
    void renderResurrectFlash();

    // ---- Zone discovery text ("Entering: <ZoneName>") ----
    static constexpr float ZONE_TEXT_DURATION = 5.0f;
    float zoneTextTimer_ = 0.0f;
    std::string zoneTextName_;
    std::string lastKnownZoneName_;
    uint32_t lastKnownWorldStateZoneId_ = 0;
    void renderZoneText(game::GameHandler& gameHandler);
};

} // namespace ui
} // namespace wowee
