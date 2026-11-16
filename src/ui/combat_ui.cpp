// ============================================================
// CombatUI - extracted from GameScreen
// Owns all combat-related UI rendering: cast bar, cooldown tracker,
// raid warning overlay, combat text, buff bar,
// battleground score HUD, combat log, threat window, BG scoreboard.
// ============================================================
#include "core/local_time.hpp"
#include "ui/combat_ui.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/buff_bar_layout.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/settings_panel.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "game/game_handler.hpp"
#include "game/bg_score_defs.hpp"
#include "game/combat_text_filter.hpp"
#include "game/interface_text.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;
    constexpr auto& kColorRed         = kRed;
    constexpr auto& kColorGreen       = kGreen;
    constexpr auto& kColorBrightGreen = kBrightGreen;
} // anonymous namespace

namespace wowee {
namespace ui {


// ============================================================
// Cast Bar
// ============================================================




// ============================================================
// Raid Warning / Boss Emote Center-Screen Overlay
// ============================================================

// ============================================================================
// The sound a whisper makes
// ============================================================================
//
// What is left of the raid warning overlay. RaidWarningFrame is FrameXML's and
// answers CHAT_MSG_RAID_WARNING itself, so the banner went; the walk through
// new chat lines stayed, because the whisper sound was being played from
// inside it and belongs to no element - FrameXML plays none for a whisper.
void CombatUI::playSoundsForNewChat(game::GameHandler& gameHandler) {
    const auto& chatHistory = gameHandler.getChatHistory();
    const size_t newCount = chatHistory.size();
    if (newCount <= raidWarnChatSeenCount_) return;

    for (size_t i = raidWarnChatSeenCount_; i < newCount; ++i) {
        if (chatHistory[i].type != game::ChatType::WHISPER) continue;
        if (auto* ac = services_.audioCoordinator) {
            if (auto* ui = ac->getUiSoundManager()) ui->playWhisperReceived();
        }
    }
    raidWarnChatSeenCount_ = newCount;
}


// ============================================================
// Floating Combat Text
// ============================================================

// The engine's numbers over the world, not Blizzard_CombatText's column.
//
// The client draws a number over a unit the player (or the player's pet) hurt
// or healed, and over no other: its three floaters - 0x0071f990 for a miss,
// block or the like, 0x00722340 for damage, 0x00722440 for healing - each
// return first when the unit is the active player, and are gated by
// CombatDamage, CombatLogPeriodicSpells, PetMeleeDamage, PetSpellDamage and
// CombatHealing, never by enableCombatText. What happens *to* the character -
// damage and heals taken, "You Dodge", power gained, honour, a reactive
// proc - is Blizzard_CombatText's, which COMBAT_TEXT_UPDATE feeds
// (combat_log_feed.cpp). That addon is not waiting for a player to touch the
// float-mode dropdown: the "Show Combat Text" checkbox loads it as the options
// panel initialises, and enableCombatText is registered on, so it is up from
// the first frame in the world.
//
// This drew every entry the handler kept, the player's own included, over the
// character or in a screen-space column - so each hit taken was shown twice,
// once in the addon's column and once here, and the takeover's sweep reported
// 'CombatText' as on screen with nothing accounting for it. What the handler
// keeps is filtered by those cvars already (combat_text_filter.hpp); what is
// left to decide here is only whose numbers these are.
void CombatUI::renderCombatText(game::GameHandler& gameHandler) {
    const auto& entries = gameHandler.getCombatText();
    if (entries.empty()) return;

    auto* window = services_.window;
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    // Camera for world-space projection
    auto* appRenderer = services_.renderer;
    rendering::Camera* camera = appRenderer ? appRenderer->getCamera() : nullptr;
    glm::mat4 viewProj;
    if (camera) viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float baseFontSize = ImGui::GetFontSize();

    // Over the unit or not at all: a unit not in the world has nothing for the
    // number to stand over, and the client has no screen-space fallback.
    if (!camera) return;

    // Only the floaters are kept (CombatHandler::addCombatText gates them as
    // 0x0071f990, 0x00722340 and 0x00722440 do; addWorldText adds the
    // client's own lines), so each is drawn over the unit it names.
    for (const auto& entry : entries) {
        const float alpha = 1.0f - (entry.age / game::CombatTextEntry::LIFETIME);
        using T = game::CombatTextEntry;

        // The text as the client builds it: damage "%d" (0x00722340), healing
        // "+%d" (0x00722440), a miss and its kin the global string of the
        // table at 0x00adbfdc (0x0071f990), world text as given.
        ImVec4 color;
        std::string text;
        switch (entry.type) {
            case T::MELEE_DAMAGE: case T::SPELL_DAMAGE: case T::PERIODIC_DAMAGE:
            case T::GLANCING: case T::CRUSHING:
                text = std::to_string(entry.amount);
                color = ImVec4(1.0f, 1.0f, 0.3f, alpha);
                break;
            case T::CRIT_DAMAGE:
                text = std::to_string(entry.amount);
                color = ImVec4(1.0f, 0.8f, 0.0f, alpha);
                break;
            case T::HEAL: case T::CRIT_HEAL: case T::PERIODIC_HEAL:
                text = "+" + std::to_string(entry.amount);
                color = ImVec4(0.3f, 1.0f, 0.3f, alpha);
                break;
            case T::WORLD_TEXT:
                text = entry.text;
                switch (static_cast<game::FloaterKind>(entry.worldKind)) {
                    case game::FloaterKind::Experience: color = ImVec4(0.7f, 0.3f, 1.0f, alpha); break;
                    case game::FloaterKind::Honor:      color = ImVec4(1.0f, 0.85f, 0.0f, alpha); break;
                    case game::FloaterKind::Threat:     color = ImVec4(1.0f, 0.3f, 0.3f, alpha); break;
                    default:                            color = ImVec4(1.0f, 1.0f, 1.0f, alpha); break;
                }
                break;
            default:
                if (const char* key = game::missFloaterKey(entry.type)) {
                    text = game::interfaceString(key, nullptr);
                    color = ImVec4(0.7f, 0.7f, 0.7f, alpha);
                }
                break;
        }
        if (text.empty()) continue;

        // --- Rendering style ---
        bool isCrit = (entry.type == game::CombatTextEntry::CRIT_DAMAGE ||
                       entry.type == game::CombatTextEntry::CRIT_HEAL);
        float renderFontSize = isCrit ? baseFontSize * 1.35f : baseFontSize;

        ImU32 shadowCol = IM_COL32(0, 0, 0, static_cast<int>(alpha * 180));
        ImU32 textCol   = ImGui::ColorConvertFloat4ToU32(color);

        // Over the unit it landed on.
        {
            // Look up the destination entity's render position
            glm::vec3 renderPos;
            bool havePos = core::Application::getInstance().getRenderPositionForGuid(entry.dstGuid, renderPos);
            if (!havePos) {
                // Fallback to entity canonical position
                auto entity = gameHandler.getEntityManager().getEntity(entry.dstGuid);
                if (entity) {
                    auto* unit = entity->isUnit() ? static_cast<game::Unit*>(entity.get()) : nullptr;
                    if (unit) {
                        renderPos = core::coords::canonicalToRender(
                            glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
                        havePos = true;
                    }
                }
            }

            if (havePos) {
                // Float upward from above the entity's head
                renderPos.z += 2.5f + entry.age * 1.2f;

                // Project to screen
                glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
                if (clipPos.w > 0.01f) {
                    glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                    if (ndc.x >= -1.5f && ndc.x <= 1.5f && ndc.y >= -1.5f && ndc.y <= 1.5f) {
                        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
                        float sy = (ndc.y * 0.5f + 0.5f) * screenH;

                        // Horizontal stagger using the random seed
                        sx += entry.xSeed * 40.0f;

                        // Center the text horizontally on the projected point
                        ImVec2 ts = font->CalcTextSizeA(renderFontSize, FLT_MAX, 0.0f, text.c_str());
                        sx -= ts.x * 0.5f;

                        // Clamp to screen bounds
                        sx = std::max(2.0f, std::min(sx, screenW - ts.x - 2.0f));

                        drawList->AddText(font, renderFontSize,
                                          ImVec2(sx + 1.0f, sy + 1.0f), shadowCol, text.c_str());
                        drawList->AddText(font, renderFontSize,
                                          ImVec2(sx, sy), textCol, text.c_str());
                    }
                }
            }
        }
    }
}

// ─── Combat Log Window ────────────────────────────────────────────────────────
void CombatUI::renderCombatLog(game::GameHandler& gameHandler,
                              SpellbookScreen& spellbookScreen) {
    if (!showCombatLog_) return;

    const auto& log = gameHandler.getCombatLog();

    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(160, 200), ImGuiCond_FirstUseEver);

    char title[64];
    snprintf(title, sizeof(title), "Combat Log (%zu)###CombatLog", log.size());
    if (!ImGui::Begin(title, &showCombatLog_)) {
        ImGui::End();
        return;
    }

    // Filter toggles
    static bool filterDamage  = true;
    static bool filterHeal    = true;
    static bool filterMisc    = true;
    static bool autoScroll    = true;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
    ImGui::Checkbox("Damage",  &filterDamage); ImGui::SameLine();
    ImGui::Checkbox("Healing", &filterHeal);   ImGui::SameLine();
    ImGui::Checkbox("Misc",    &filterMisc);   ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.0f);
    if (ImGui::SmallButton("Clear"))
        gameHandler.clearCombatLog();
    ImGui::PopStyleVar();
    ImGui::Separator();

    // Helper: categorize entry
    auto isDamageType = [](game::CombatTextEntry::Type t) {
        using T = game::CombatTextEntry;
        return t == T::MELEE_DAMAGE || t == T::SPELL_DAMAGE ||
               t == T::CRIT_DAMAGE  || t == T::PERIODIC_DAMAGE ||
               t == T::ENVIRONMENTAL || t == T::GLANCING || t == T::CRUSHING;
    };
    auto isHealType = [](game::CombatTextEntry::Type t) {
        using T = game::CombatTextEntry;
        return t == T::HEAL || t == T::CRIT_HEAL || t == T::PERIODIC_HEAL;
    };

    // Two-column table: Time | Event description
    ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingFixedFit;
    float availH = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("##CombatLogTable", 2, tableFlags, ImVec2(0.0f, availH))) {
        ImGui::TableSetupScrollFreeze(0, 0);
        ImGui::TableSetupColumn("Time",  ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);

        for (const auto& e : log) {
            // Apply filters
            bool isDmg  = isDamageType(e.type);
            bool isHeal = isHealType(e.type);
            bool isMisc = !isDmg && !isHeal;
            if (isDmg  && !filterDamage) continue;
            if (isHeal && !filterHeal)   continue;
            if (isMisc && !filterMisc)   continue;

            // Format timestamp as HH:MM:SS
            char timeBuf[10];
            {
                const std::tm tm_info = core::localTime(e.timestamp);
                snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                         tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
            }

            // Build event description and choose color
            char desc[256];
            ImVec4 color;
            using T = game::CombatTextEntry;
            const char* src = e.sourceName.empty() ? (e.isPlayerSource ? "You" : "?") : e.sourceName.c_str();
            const char* tgt = e.targetName.empty() ? "?" : e.targetName.c_str();
            const std::string& spellName = (e.spellId != 0) ? gameHandler.getSpellName(e.spellId) : std::string();
            const char* spell = spellName.empty() ? nullptr : spellName.c_str();

            switch (e.type) {
                case T::MELEE_DAMAGE:
                    snprintf(desc, sizeof(desc), "%s hits %s for %d", src, tgt, e.amount);
                    color = e.isPlayerSource ? ImVec4(1.0f, 0.9f, 0.3f, 1.0f) : colors::kSoftRed;
                    break;
                case T::CRIT_DAMAGE:
                    snprintf(desc, sizeof(desc), "%s crits %s for %d!", src, tgt, e.amount);
                    color = e.isPlayerSource ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) : colors::kBrightRed;
                    break;
                case T::SPELL_DAMAGE:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s's %s hits %s for %d", src, spell, tgt, e.amount);
                    else
                        snprintf(desc, sizeof(desc), "%s's spell hits %s for %d", src, tgt, e.amount);
                    color = e.isPlayerSource ? ImVec4(1.0f, 0.9f, 0.3f, 1.0f) : colors::kSoftRed;
                    break;
                case T::PERIODIC_DAMAGE:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s's %s ticks %s for %d", src, spell, tgt, e.amount);
                    else
                        snprintf(desc, sizeof(desc), "%s's DoT ticks %s for %d", src, tgt, e.amount);
                    color = e.isPlayerSource ? ImVec4(0.9f, 0.7f, 0.3f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
                    break;
                case T::HEAL:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s heals %s for %d (%s)", src, tgt, e.amount, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s heals %s for %d", src, tgt, e.amount);
                    color = kColorGreen;
                    break;
                case T::CRIT_HEAL:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s critically heals %s for %d! (%s)", src, tgt, e.amount, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s critically heals %s for %d!", src, tgt, e.amount);
                    color = kColorBrightGreen;
                    break;
                case T::PERIODIC_HEAL:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s's %s heals %s for %d", src, spell, tgt, e.amount);
                    else
                        snprintf(desc, sizeof(desc), "%s's HoT heals %s for %d", src, tgt, e.amount);
                    color = ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
                    break;
                case T::MISS:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s's %s misses %s", src, spell, tgt);
                    else
                        snprintf(desc, sizeof(desc), "%s misses %s", src, tgt);
                    color = colors::kMediumGray;
                    break;
                case T::DODGE:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s dodges %s's %s", tgt, src, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s dodges %s's attack", tgt, src);
                    color = colors::kMediumGray;
                    break;
                case T::PARRY:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s parries %s's %s", tgt, src, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s parries %s's attack", tgt, src);
                    color = colors::kMediumGray;
                    break;
                case T::BLOCK:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s blocks %s's %s (%d blocked)", tgt, src, spell, e.amount);
                    else
                        snprintf(desc, sizeof(desc), "%s blocks %s's attack (%d blocked)", tgt, src, e.amount);
                    color = ImVec4(0.65f, 0.75f, 0.65f, 1.0f);
                    break;
                case T::EVADE:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s evades %s's %s", tgt, src, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s evades %s's attack", tgt, src);
                    color = colors::kMediumGray;
                    break;
                case T::IMMUNE:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s is immune to %s", tgt, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s is immune", tgt);
                    color = colors::kSilver;
                    break;
                case T::ABSORB:
                    if (spell && e.amount > 0)
                        snprintf(desc, sizeof(desc), "%s's %s absorbs %d", src, spell, e.amount);
                    else if (spell)
                        snprintf(desc, sizeof(desc), "%s absorbs %s", tgt, spell);
                    else if (e.amount > 0)
                        snprintf(desc, sizeof(desc), "%d absorbed", e.amount);
                    else
                        snprintf(desc, sizeof(desc), "Absorbed");
                    color = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
                    break;
                case T::RESIST:
                    if (spell && e.amount > 0)
                        snprintf(desc, sizeof(desc), "%s resists %s's %s (%d resisted)", tgt, src, spell, e.amount);
                    else if (spell)
                        snprintf(desc, sizeof(desc), "%s resists %s's %s", tgt, src, spell);
                    else if (e.amount > 0)
                        snprintf(desc, sizeof(desc), "%d resisted", e.amount);
                    else
                        snprintf(desc, sizeof(desc), "Resisted");
                    color = ImVec4(0.6f, 0.6f, 0.9f, 1.0f);
                    break;
                case T::DEFLECT:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s deflects %s's %s", tgt, src, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s deflects %s's attack", tgt, src);
                    color = ImVec4(0.65f, 0.8f, 0.95f, 1.0f);
                    break;
                case T::REFLECT:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s reflects %s's %s", tgt, src, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s reflects %s's attack", tgt, src);
                    color = ImVec4(0.8f, 0.7f, 1.0f, 1.0f);
                    break;
                case T::ENVIRONMENTAL: {
                    const char* envName = "Environmental";
                    switch (e.powerType) {
                        case 0: envName = "Fatigue"; break;
                        case 1: envName = "Drowning"; break;
                        case 2: envName = "Falling"; break;
                        case 3: envName = "Lava"; break;
                        case 4: envName = "Slime"; break;
                        case 5: envName = "Fire"; break;
                    }
                    snprintf(desc, sizeof(desc), "%s damage: %d", envName, e.amount);
                    color = ImVec4(1.0f, 0.5f, 0.2f, 1.0f);
                    break;
                }
                case T::ENERGIZE: {
                    const char* pwrName = game::powerTypeName(e.powerType);
                    if (!pwrName) pwrName = "power";
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s gains %d %s (%s)", tgt, e.amount, pwrName, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s gains %d %s", tgt, e.amount, pwrName);
                    color = colors::kLightBlue;
                    break;
                }
                case T::POWER_DRAIN: {
                    const char* drainName = game::powerTypeName(e.powerType);
                    if (!drainName) drainName = "power";
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s loses %d %s to %s's %s", tgt, e.amount, drainName, src, spell);
                    else
                        snprintf(desc, sizeof(desc), "%s loses %d %s", tgt, e.amount, drainName);
                    color = ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
                    break;
                }
                case T::XP_GAIN:
                    snprintf(desc, sizeof(desc), "You gain %d experience", e.amount);
                    color = ImVec4(0.8f, 0.6f, 1.0f, 1.0f);
                    break;
                case T::PROC_TRIGGER:
                    if (spell)
                        snprintf(desc, sizeof(desc), "%s procs!", spell);
                    else
                        snprintf(desc, sizeof(desc), "Proc triggered");
                    color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
                    break;
                case T::DISPEL:
                    if (spell && e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You dispel %s from %s", spell, tgt);
                    else if (spell)
                        snprintf(desc, sizeof(desc), "%s dispels %s from %s", src, spell, tgt);
                    else if (e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You dispel from %s", tgt);
                    else
                        snprintf(desc, sizeof(desc), "%s dispels from %s", src, tgt);
                    color = ImVec4(0.6f, 0.9f, 1.0f, 1.0f);
                    break;
                case T::STEAL:
                    if (spell && e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You steal %s from %s", spell, tgt);
                    else if (spell)
                        snprintf(desc, sizeof(desc), "%s steals %s from %s", src, spell, tgt);
                    else if (e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You steal from %s", tgt);
                    else
                        snprintf(desc, sizeof(desc), "%s steals from %s", src, tgt);
                    color = ImVec4(0.8f, 0.7f, 1.0f, 1.0f);
                    break;
                case T::INTERRUPT:
                    if (spell && e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You interrupt %s's %s", tgt, spell);
                    else if (spell)
                        snprintf(desc, sizeof(desc), "%s interrupts %s's %s", src, tgt, spell);
                    else if (e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You interrupt %s", tgt);
                    else
                        snprintf(desc, sizeof(desc), "%s interrupted", tgt);
                    color = ImVec4(1.0f, 0.6f, 0.9f, 1.0f);
                    break;
                case T::INSTAKILL:
                    if (spell && e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You instantly kill %s with %s", tgt, spell);
                    else if (spell)
                        snprintf(desc, sizeof(desc), "%s instantly kills %s with %s", src, tgt, spell);
                    else if (e.isPlayerSource)
                        snprintf(desc, sizeof(desc), "You instantly kill %s", tgt);
                    else
                        snprintf(desc, sizeof(desc), "%s instantly kills %s", src, tgt);
                    color = colors::kBrightRed;
                    break;
                case T::HONOR_GAIN:
                    snprintf(desc, sizeof(desc), "You gain %d honor", e.amount);
                    color = colors::kBrightGold;
                    break;
                case T::GLANCING:
                    snprintf(desc, sizeof(desc), "%s glances %s for %d", src, tgt, e.amount);
                    color = e.isPlayerSource ? ImVec4(0.75f, 0.75f, 0.5f, 1.0f)
                                             : ImVec4(0.75f, 0.4f, 0.4f, 1.0f);
                    break;
                case T::CRUSHING:
                    snprintf(desc, sizeof(desc), "%s crushes %s for %d!", src, tgt, e.amount);
                    color = e.isPlayerSource ? ImVec4(1.0f, 0.55f, 0.1f, 1.0f)
                                             : ImVec4(1.0f, 0.15f, 0.15f, 1.0f);
                    break;
                default:
                    snprintf(desc, sizeof(desc), "Combat event (type %d, amount %d)", static_cast<int>(e.type), e.amount);
                    color = ui::colors::kLightGray;
                    break;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", timeBuf);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(color, "%s", desc);
            // Hover tooltip: show rich spell info for entries with a known spell
            if (e.spellId != 0 && ImGui::IsItemHovered()) {
                auto* assetMgrLog = services_.assetManager;
                ImGui::BeginTooltip();
                bool richOk = spellbookScreen.renderSpellInfoTooltip(e.spellId, gameHandler, assetMgrLog);
                if (!richOk) {
                    ImGui::Text("%s", spellName.c_str());
                }
                ImGui::EndTooltip();
            }
        }

        // Auto-scroll to bottom
        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndTable();
    }

    ImGui::End();
}


// ─── Threat Window ────────────────────────────────────────────────────────────
void CombatUI::renderThreatWindow(game::GameHandler& gameHandler) {
    if (!showThreatWindow_) return;

    const auto* list = gameHandler.getTargetThreatList();

    ImGui::SetNextWindowSize(ImVec2(280, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);

    if (!ImGui::Begin("Threat###ThreatWin", &showThreatWindow_,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    if (!list || list->empty()) {
        ImGui::TextDisabled("No threat data for current target.");
        ImGui::End();
        return;
    }

    uint32_t maxThreat = list->front().threat;

    // Pre-scan to find the player's rank and threat percentage
    uint64_t playerGuid = gameHandler.getPlayerGuid();
    int playerRank = 0;
    float playerPct = 0.0f;
    {
        int scan = 0;
        for (const auto& e : *list) {
            ++scan;
            if (e.victimGuid == playerGuid) {
                playerRank = scan;
                playerPct  = (maxThreat > 0) ? static_cast<float>(e.threat) / static_cast<float>(maxThreat) : 0.0f;
                break;
            }
            if (scan >= 10) break;
        }
    }

    // Status bar: aggro alert or rank summary
    if (playerRank == 1) {
        // Player has aggro - persistent red warning
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "!! YOU HAVE AGGRO !!");
    } else if (playerRank > 1 && playerPct >= 0.8f) {
        // Close to pulling - pulsing warning
        float pulse = 0.55f + 0.45f * sinf(static_cast<float>(ImGui::GetTime()) * 5.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, pulse), "! PULLING AGGRO (%.0f%%) !", playerPct * 100.0f);
    } else if (playerRank > 0) {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "You: #%d  %.0f%% threat", playerRank, playerPct * 100.0f);
    }

    ImGui::TextDisabled("%-19s  Threat", "Player");
    ImGui::Separator();

    int rank = 0;
    for (const auto& entry : *list) {
        ++rank;
        bool isPlayer = (entry.victimGuid == playerGuid);

        // Resolve name
        std::string victimName;
        auto entity = gameHandler.getEntityManager().getEntity(entry.victimGuid);
        if (entity) {
            if (entity->getType() == game::ObjectType::PLAYER) {
                auto p = std::static_pointer_cast<game::Player>(entity);
                victimName = p->getName().empty() ? "Player" : p->getName();
            } else if (entity->getType() == game::ObjectType::UNIT) {
                auto u = std::static_pointer_cast<game::Unit>(entity);
                victimName = u->getName().empty() ? "NPC" : u->getName();
            }
        }
        if (victimName.empty())
            victimName = "0x" + [&](){
                char buf[20]; snprintf(buf, sizeof(buf), "%llX",
                    static_cast<unsigned long long>(entry.victimGuid)); return std::string(buf); }();

        // Colour: gold for #1 (tank), red if player is highest, white otherwise
        ImVec4 col = ui::colors::kWhite;
        if (rank == 1) col = ui::colors::kTooltipGold;      // gold
        if (isPlayer && rank == 1) col = kColorRed; // red - you have aggro

        // Threat bar
        float pct = (maxThreat > 0) ? static_cast<float>(entry.threat) / static_cast<float>(maxThreat) : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
            isPlayer ? ImVec4(0.8f, 0.2f, 0.2f, 0.7f) : ImVec4(0.2f, 0.5f, 0.8f, 0.5f));
        char barLabel[48];
        snprintf(barLabel, sizeof(barLabel), "%.0f%%", pct * 100.0f);
        ImGui::ProgressBar(pct, ImVec2(60, 14), barLabel);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::TextColored(col, "%-18s  %u", victimName.c_str(), entry.threat);

        if (rank >= 10) break; // cap display at 10 entries
    }

    ImGui::End();
}



} // namespace ui
} // namespace wowee
