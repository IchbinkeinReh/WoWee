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
#include "game/transport_manager.hpp"
#include "ui/ui_helpers.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "game/game_handler.hpp"
#include "game/bg_score_defs.hpp"
#include "game/combat_text_filter.hpp"
#include "game/world_text.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/ui_texture_load.hpp"
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
#include <unordered_map>

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
    namespace wt = game::world_text;
    const auto& entries = gameHandler.getCombatText();
    if (entries.empty()) return;

    auto* window = services_.window;
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    // Camera for world-space projection
    auto* appRenderer = services_.renderer;
    rendering::Camera* camera = appRenderer ? appRenderer->getCamera() : nullptr;
    // Over the unit or not at all: a unit not in the world has nothing for the
    // number to stand over, and the client has no screen-space fallback.
    if (!camera) return;
    const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    // The plane a tenth of the view's height under its top (0x007e5c30,
    // wt::topOfViewPlane), inside where a x + b y + c z + d >= 0: clip y is
    // down on the screen here, so up is -y.
    const float rowUp[4] = {-viewProj[0][1], -viewProj[1][1], -viewProj[2][1], -viewProj[3][1]};
    const float rowW[4] = {viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]};
    float topPlane[4];
    wt::topOfViewPlane(rowUp, rowW, topPlane);
    // The transport the player stands on (the camera's, +0xa0, a game
    // object): lines are kept in its frame and drawn in it (0x007e5c30,
    // 0x007e70d0).
    const glm::mat4* transportFrame = nullptr;
    if (auto* transports = gameHandler.getTransportManager(); transports && gameHandler.isOnTransport()) {
        if (const auto* t = transports->getTransport(gameHandler.getPlayerTransportGuid())) transportFrame = &t->transform;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    auto& app = core::Application::getInstance();

    // First person: the camera this close to its unit (0x005fe3a0).
    const auto* cc = appRenderer->getCameraController();
    const bool firstPerson = cc && cc->getCurrentDistance() < wt::kFirstPersonDistance;
    const uint64_t cameraUnit = gameHandler.cameraTarget();

    // Lines standing over each unit now, for the four the client keeps.
    std::unordered_map<uint64_t, int> standing;
    for (const auto& entry : entries)
        if (entry.anchored && !entry.dropped) ++standing[entry.dstGuid];

    // Only the floaters are kept (CombatHandler::addCombatText gates them as
    // 0x0071f990, 0x00722340 and 0x00722440 do; addWorldText adds the
    // client's own lines), so each is drawn over the unit it names.
    for (const auto& entry : entries) {
        using T = game::CombatTextEntry;
        if (entry.dropped) continue;

        // Placed once, as the line is made (0x007e6030 -> 0x007e5c30): none
        // over the camera's unit in first person, none past four over one
        // unit, and the rest under the unit's name position by a third of a
        // yard, where it stays.
        if (!entry.anchored) {
            entry.anchored = true;
            if ((firstPerson && entry.dstGuid == cameraUnit) || standing[entry.dstGuid] >= wt::kMaxPerUnit) {
                entry.dropped = true;
                continue;
            }
            glm::vec3 at(0.0f);
            if (auto name = app.getUnitNamePosition(entry.dstGuid)) {
                at = *name;
            } else if (glm::vec3 pos; app.getRenderPositionForGuid(entry.dstGuid, pos)) {
                at = pos + glm::vec3(0.0f, 0.0f, 2.3f);
            } else {
                entry.dropped = true;
                continue;
            }
            at.z -= wt::kBelowName;
            // Under the top of the view, room left for its rise; then into
            // the transport's frame.
            at.z = wt::zUnderTopOfView(topPlane, at.x, at.y, at.z, entry.kind);
            if (transportFrame) at = glm::vec3(glm::inverse(*transportFrame) * glm::vec4(at, 1.0f));
            entry.anchor[0] = at.x;
            entry.anchor[1] = at.y;
            entry.anchor[2] = at.z;
            ++standing[entry.dstGuid];
        }

        const uint32_t ms = static_cast<uint32_t>(entry.age * 1000.0f);
        if (wt::expired(entry.kind, ms)) continue;
        const wt::Alpha alpha = wt::alphaAt(entry.kind, ms);
        const float a = static_cast<float>(alpha.text) / 255.0f;

        // The text as the client builds it: damage "%d" (0x00722340), healing
        // "+%d" (0x00722440), a miss and its kin the global string of the
        // table at 0x00adbfdc (0x0071f990), world text as given.
        // Its maker's colour, else its kind's (0x007e6dc0, the table at
        // 0x00af4760); the fade sets the alpha (0x007e6850).
        const uint32_t argb = entry.colour != 0 ? entry.colour : wt::colourOf(entry.kind);
        const ImVec4 color(static_cast<float>((argb >> 16) & 0xffu) / 255.0f,
                           static_cast<float>((argb >> 8) & 0xffu) / 255.0f,
                           static_cast<float>(argb & 0xffu) / 255.0f, a);
        std::string text;
        switch (entry.type) {
            case T::MELEE_DAMAGE: case T::SPELL_DAMAGE: case T::PERIODIC_DAMAGE:
            case T::GLANCING: case T::CRUSHING: case T::CRIT_DAMAGE:
                text = std::to_string(entry.amount);
                break;
            case T::HEAL: case T::CRIT_HEAL: case T::PERIODIC_HEAL:
                text = "+" + std::to_string(entry.amount);
                break;
            case T::WORLD_TEXT:
                text = entry.text;
                break;
            default:
                if (const char* key = game::missFloaterKey(entry.type)) text = game::interfaceString(key, nullptr);
                break;
        }
        if (text.empty()) continue;

        // Its height by its kind, the crits popping (0x007e6cc0), in pixels of
        // a screen whose diagonal is one interface unit (0x007e66f0).
        const float fontPx = wt::pixels(wt::scaleAt(entry.kind, ms), screenW, screenH);
        // Straight up from where it was put by its kind's rise (0x007e6590),
        // then onto the screen and kept inside it, centred (0x007e70d0).
        // Out of the transport's frame, whichever the player is on now, then
        // up by its rise.
        glm::vec3 at(entry.anchor[0], entry.anchor[1], entry.anchor[2]);
        if (transportFrame) at = glm::vec3(*transportFrame * glm::vec4(at, 1.0f));
        at.z += wt::riseAt(entry.kind, ms);
        const glm::vec4 clip = viewProj * glm::vec4(at, 1.0f);
        if (clip.w <= 0.01f) continue;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        const ImVec2 ts = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, text.c_str());
        const float cx = wt::clampCentre((ndc.x * 0.5f + 0.5f) * screenW, ts.x * 0.5f, screenW);
        const float cy = wt::clampCentre((ndc.y * 0.5f + 0.5f) * screenH, ts.y * 0.5f, screenH);
        const float left = cx - ts.x * 0.5f;
        const float top = cy - ts.y * 0.5f;

        const ImU32 shadowCol = IM_COL32(0, 0, 0, alpha.shadow);
        const ImU32 textCol = ImGui::ColorConvertFloat4ToU32(color);
        drawList->AddText(font, fontPx, ImVec2(left + 1.0f, top + 1.0f), shadowCol, text.c_str());
        drawList->AddText(font, fontPx, ImVec2(left, top), textCol, text.c_str());

        // An honourable kill's line carries the victim's rank badge, as tall
        // as the text, 40 pixels left of it (0x007e7390, 0x007e7490).
        if (entry.kind == static_cast<uint8_t>(game::FloaterKind::Honor)) {
            if (const uint32_t badge = wt::rankBadge(entry.rank)) {
                char path[64];
                std::snprintf(path, sizeof(path), "Interface\\PvPRankBadges\\PvPRank%02u.blp", static_cast<unsigned>(badge));
                if (VkDescriptorSet tex = rankBadgeTexture(path)) {
                    const float side = ts.y;
                    drawList->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(left - 40.0f, top),
                                       ImVec2(left - 40.0f + side, top + side), ImVec2(0, 0), ImVec2(1, 1),
                                       IM_COL32(255, 255, 255, alpha.text));
                }
            }
        }
    }
}

VkDescriptorSet CombatUI::rankBadgeTexture(const char* path) {
    // Loaded once each (0x007e7c20 loads all fifteen up front).
    if (ui::uiTexturesReplaced(rankBadgeGenerationSeen_)) rankBadges_.clear();
    auto it = rankBadges_.find(path);
    if (it != rankBadges_.end()) return it->second;
    VkDescriptorSet tex = ui::uploadUiTextureFromBlp(services_.assetManager, path, services_.window, nullptr);
    rankBadges_[path] = tex;
    return tex;
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
