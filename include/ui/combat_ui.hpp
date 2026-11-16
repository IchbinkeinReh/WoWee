#pragma once

#include "ui/ui_services.hpp"
#include <imgui.h>
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace wowee {
namespace game { class GameHandler; }
namespace pipeline { class AssetManager; }
namespace ui {

class SettingsPanel;
class SpellbookScreen;
class InventoryScreen;

/**
 * Combat UI overlay manager (extracted from GameScreen)
 *
 * Owns all combat-related rendering:
 *   cast bar, cooldown tracker, raid warning overlay, floating combat text,
 *   buff bar, battleground score HUD, combat log,
 *   threat window, BG scoreboard.
 */
class CombatUI {
public:
    CombatUI() = default;

    // ---- Callback type for spell icon lookup (stays in GameScreen) ----
    using SpellIconFn = std::function<VkDescriptorSet(uint32_t spellId, pipeline::AssetManager*)>;

    // ---- Toggle booleans (written by slash commands / escape handler / settings) ----
    bool showCombatLog_ = false;
    bool showThreatWindow_ = false;

    size_t raidWarnChatSeenCount_ = 0;

    // ---- Public render methods ----
    /// The whisper sound, taken from a walk of new chat lines. What is left
    /// of the raid warning overlay, whose banner is FrameXML's.
    void playSoundsForNewChat(game::GameHandler& gameHandler);
    void renderCombatText(game::GameHandler& gameHandler);
    // inventoryScreen supplies the weapon item icons shown for temporary weapon enchants.
    void renderCombatLog(game::GameHandler& gameHandler,
                         SpellbookScreen& spellbookScreen);
    void renderThreatWindow(game::GameHandler& gameHandler);

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

private:
    UIServices services_;
    /// The PvP rank badges an honourable kill's line carries, by path.
    VkDescriptorSet rankBadgeTexture(const char* path);
    std::unordered_map<std::string, VkDescriptorSet> rankBadges_;
    uint32_t rankBadgeGenerationSeen_ = 0;  ///< see uiTexturesReplaced
};

} // namespace ui
} // namespace wowee
