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
 * What is left of this client's own notifications: the sounds a level-up
 * and an achievement make, and the zone text for when FrameXML does not
 * draw its own. The client has no toast, no "Level N!" flash, no ding banner
 * and no resurrection flash: a level gained is chatframe.lua's LEVEL_UP lines
 * on PLAYER_LEVEL_UP, and coming back to life shows nothing but the world.
 */
class ToastManager {
public:
    ToastManager() = default;

    /// Render the zone text - called after the escape menu
    void renderLateToasts(game::GameHandler& gameHandler);

    /// The level-up sound. Nothing is drawn: the level is announced in chat
    /// by FrameXML, and LevelUp.m2 is the renderer's.
    void playLevelUp();

    /// Fire achievement earned toast + sound
    /// The sound an achievement makes. The badge is FrameXML's.
    void playAchievementEarned();

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

private:
    // Injected UI services
    UIServices services_;

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
