// Names over heads and nameplates, drawn apart as the client draws them.
//
// The client has two things here. PlayerName.cpp (0x007e5640) writes a unit's
// name as world-size text over its PlayerName attachment, coloured by how it
// regards the player, for the units the UnitName* cvars ask for (0x00729c70).
// NamePlateFrame.cpp (0x0098f790) is an interface frame with a health bar,
// the name, the level, an elite dragon, a raid icon and, for the target, a
// cast bar - for the units the nameplate* cvars ask for (0x0072b060). A unit
// with a plate has no name over its head; the plate carries it.

#include "ui/game_screen.hpp"

#include "addons/lua_api_registrations.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "game/game_handler.hpp"
#include "game/unit_name_rules.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/camera.hpp"
#include "rendering/renderer.hpp"
#include "rendering/unit_name_anchor.hpp"
#include "rendering/wmo_renderer.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/nameplate_stacking.hpp"
#include "ui/sight_cache.hpp"
#include "ui/ui_helpers.hpp"
#include "ui/ui_raid_icons.hpp"
#include "ui/ui_texture_load.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee { namespace ui {

// game_screen.cpp: who may attack whom between players (0x00729740), and the
// selection colour (0x00521bf0), which is the name's colour too (0x00718ac0).
bool playerMayAttackPlayer(game::GameHandler& gameHandler, const game::Unit& other);
glm::vec4 targetCircleColor(game::GameHandler& gameHandler, const game::Unit& unit);

namespace {

namespace un = game::unit_names;
using helpers::classColorU32;
using helpers::entityClassId;

uint64_t guidAt(const game::Entity& e, game::UF low) {
    const uint16_t idx = game::fieldIndex(low);
    if (idx == 0xFFFF) return 0;
    return e.getField(idx) | (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(idx + 1))) << 32);
}

bool isPlayerGuid(uint64_t guid) { return guid != 0 && (guid >> 48) == 0; }

/// The player may attack this unit (0x00729740): for another player the duel
/// and PvP rules, for anything else not flagged out of reach and not regarded
/// by the player as friendly.
bool mayAttack(game::GameHandler& gh, const game::Unit& unit, bool isPlayer) {
    if (isPlayer) return playerMayAttackPlayer(gh, unit);
    return gh.playerMayAttackCreature(unit);
}

/// What 0x00729c70 and 0x0072b060 read off a unit.
un::UnitFacts unitFacts(game::GameHandler& gh, const game::Entity& e, const game::Unit& unit) {
    un::UnitFacts f;
    f.isPlayer = e.getType() == game::ObjectType::PLAYER;
    f.notSelectable = (unit.getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) != 0;
    f.attackable = mayAttack(gh, unit, f.isPlayer);
    const uint16_t bytes1 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1);
    f.creeping = bytes1 != 0xFFFF && ((e.getField(bytes1) >> 16) & 0x2) != 0;
    f.creatureType = f.isPlayer ? 0 : gh.getCreatureType(unit.getEntry());
    const uint64_t charmedBy = guidAt(e, game::UF::UNIT_FIELD_CHARMEDBY);
    const uint64_t summonedBy = game::unitSummonedByGuid(e);
    const uint64_t createdBy = guidAt(e, game::UF::UNIT_FIELD_CREATEDBY);
    f.charmedBy = charmedBy != 0;
    f.summonedBy = summonedBy != 0;
    f.createdBy = createdBy != 0;
    // 0x004f5f20: the charmer, else the maker.
    f.ownerIsPlayer = isPlayerGuid(charmedBy != 0 ? charmedBy : createdBy);
    f.immuneToNpc = (unit.getUnitFlags() & 0x200u) != 0;
    f.alive = unit.getHealth() >= 1 && (unit.getDynamicFlags() & game::UNIT_DYNFLAG_DEAD) == 0;
    // 0x00729b30: nothing charmed, not attackable and of the player's side.
    f.friendlyGroup = !f.charmedBy && !f.attackable && !unit.isHostile();
    if (charmedBy != 0) {
        const uint64_t me = gh.getPlayerGuid();
        if (auto charmer = gh.getEntityManager().getEntity(charmedBy); charmer && charmer->isUnit()) {
            const bool controlsIt = guidAt(*charmer, game::UF::UNIT_FIELD_CHARM) == e.getGuid();
            const auto& c = static_cast<const game::Unit&>(*charmer);
            const bool friendly = charmedBy == me ||
                (charmer->getType() == game::ObjectType::PLAYER && !playerMayAttackPlayer(gh, c) &&
                 !c.isHostile());
            f.charmerFriendly = controlsIt && friendly;
        }
    }
    return f;
}

/// 0x0098e5f0: the level's colour on a plate, against the player's level -
/// red five or more above, orange three, yellow within two, green, and grey
/// past the grey range (0x00aa34b8 by the player's level over five).
ImU32 levelColor(uint32_t unitLevel, uint32_t playerLevel, int alpha) {
    static constexpr int kGreyRange[20] = {4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    const int diff = static_cast<int>(unitLevel) - static_cast<int>(playerLevel);
    if (diff > 4) return IM_COL32(255, 25, 25, alpha);
    if (diff > 2) return IM_COL32(255, 127, 63, alpha);
    if (diff > -3) return IM_COL32(255, 255, 0, alpha);
    const uint32_t band = playerLevel / 5;
    const int grey = band < 20 ? kGreyRange[band] : 8;
    if (-diff > grey) return IM_COL32(127, 127, 127, alpha);
    return IM_COL32(63, 178, 63, alpha);
}

ImU32 toU32(const glm::vec4& c, int alpha) {
    return IM_COL32(static_cast<int>(c.r * 255.0f + 0.5f), static_cast<int>(c.g * 255.0f + 0.5f),
                    static_cast<int>(c.b * 255.0f + 0.5f), alpha);
}

/// Text with the outline UNIT_NAME_FONT is made with (flags 4, 0x007e64d0).
void outlinedText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text) {
    const ImU32 shadow = IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) & 0xFF);
    for (const ImVec2 d : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1)}) {
        dl->AddText(font, size, ImVec2(at.x + d.x, at.y + d.y), shadow, text);
    }
    dl->AddText(font, size, at, color, text);
}

/// One interface texture, loaded once; a failed load is not retried.
VkDescriptorSet plateTexture(pipeline::AssetManager* assets, const char* path) {
    static std::unordered_map<std::string, VkDescriptorSet> cache;
    if (auto it = cache.find(path); it != cache.end()) return it->second;
    VkDescriptorSet tex =
        uploadUiTextureFromBlp(assets, path, core::Application::getInstance().getWindow());
    cache[path] = tex;
    return tex;
}

/// Where a world point lands on screen, or nothing behind the camera.
std::optional<ImVec2> project(const glm::mat4& viewProj, const glm::vec3& p, float w, float h,
                              float* clipW = nullptr) {
    const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
    if (clip.w <= 0.01f) return std::nullopt;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (clipW) *clipW = clip.w;
    return ImVec2((ndc.x * 0.5f + 0.5f) * w, (ndc.y * 0.5f + 0.5f) * h);
}

}  // namespace

void GameScreen::renderNameplates(game::GameHandler& gameHandler) {
    if (gameHandler.getState() != game::WorldState::IN_WORLD) return;
    gameHandler.setMouseoverGuid(0);

    auto* appRenderer = services_.renderer;
    rendering::Camera* camera = appRenderer ? appRenderer->getCamera() : nullptr;
    auto* window = services_.window;
    if (!camera || !window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());
    const glm::mat4 proj = camera->getProjectionMatrix();
    const glm::mat4 viewProj = proj * camera->getViewMatrix();
    const glm::vec3 camPos = camera->getPosition();
    const uint64_t playerGuid = gameHandler.getPlayerGuid();
    const uint64_t targetGuid = gameHandler.getTargetGuid();
    auto& app = core::Application::getInstance();
    auto* assets = services_.assetManager;

    // The two cvar masks (0x007e6150, 0x00511xxx).
    const uint32_t nameMask = un::nameMask(
        [](const char* name, const char* def) { return addons::storedCVarValue(name, def); });
    auto cvarOn = [](const char* name, const char* def) {
        return addons::storedCVarValue(name, def) != "0";
    };
    un::PlateSwitches plates;
    plates.enemies = settingsPanel_.showEnemyNameplates_;
    plates.friends = settingsPanel_.showFriendlyNameplates_;
    plates.enemyPets = cvarOn("nameplateShowEnemyPets", "1");
    plates.enemyGuardians = cvarOn("nameplateShowEnemyGuardians", "1");
    plates.enemyTotems = cvarOn("nameplateShowEnemyTotems", "1");
    plates.friendlyPets = cvarOn("nameplateShowFriendlyPets", "1");
    plates.friendlyGuardians = cvarOn("nameplateShowFriendlyGuardians", "1");
    plates.friendlyTotems = cvarOn("nameplateShowFriendlyTotems", "1");
    const bool allowPlateOverlap = cvarOn("nameplateAllowOverlap", "1");
    const bool classColours = cvarOn("ShowClassColorInNameplate", "0");
    const bool targetCastBar = cvarOn("showVKeyCastbar", "1");

    // UNIT_NAME_FONT and NAMEPLATE_FONT are both FRIZQT__ (enUS GlobalStrings).
    ImFont* font = interfaceFaceOrDefault("Fonts\\FRIZQT__.TTF");
    // Interface units: the frame code counts the screen's height as 0.75 of
    // a 1024-wide unit (0x00b2da70/74), and the interface draws 768 to it.
    const float ui = screenH / 768.0f * 1024.0f;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    static thread_local std::vector<std::shared_ptr<game::Entity>> nearby;
    glm::vec3 playerCanonical(0.0f);
    if (auto player = gameHandler.getEntityManager().getEntity(playerGuid))
        playerCanonical = glm::vec3(player->getX(), player->getY(), player->getZ());
    gameHandler.getEntityManager().getEntitiesNear(playerCanonical.x, playerCanonical.y, 150.0f, nearby);
    const glm::vec3 playerRenderPos = core::coords::canonicalToRender(playerCanonical);

    // Nearest first, so with overlap off the plate that moves is the farther.
    std::sort(nearby.begin(), nearby.end(), [&](const auto& a, const auto& b) {
        if (!a || !b) return a != nullptr;
        const glm::vec3 da(a->getX() - playerCanonical.x, a->getY() - playerCanonical.y, a->getZ() - playerCanonical.z);
        const glm::vec3 db(b->getX() - playerCanonical.x, b->getY() - playerCanonical.y, b->getZ() - playerCanonical.z);
        return glm::dot(da, da) < glm::dot(db, db);
    });
    static thread_local std::vector<ui::PlateBox> placed;
    placed.clear();
    static SightCache sight;
    const auto* wmo = appRenderer ? appRenderer->queryWMORenderer() : nullptr;

    // The name's anchor: the PlayerName attachment, else over the model
    // (0x0071fef0).
    auto namePoint = [&](uint64_t guid, const game::Entity& e) {
        if (auto p = app.getUnitNamePosition(guid)) return *p;
        glm::vec3 base;
        if (!app.getRenderPositionForGuid(guid, base))
            base = core::coords::canonicalToRender(glm::vec3(e.getX(), e.getY(), e.getZ()));
        return base + glm::vec3(0.0f, 0.0f, 2.3f);
    };

    // A name over a head: world-size text whose block stands on the anchor
    // (0x007e5640), depth tested in the world - a wall in front hides it.
    auto drawOverheadName = [&](uint64_t guid, const game::Entity& e, const game::Unit& unit, bool isPlayer) {
        const glm::vec3 anchor = namePoint(guid, e);
        if (guid != playerGuid && sight.blocked(wmo, guid, camPos, anchor)) return;
        float clipW = 0.0f;
        const auto at = project(viewProj, anchor, screenW, screenH, &clipW);
        if (!at) return;

        // 0x0072d4f0: tags, the name with its title, then the guild or the
        // creature's title on lines of their own.
        std::string text;
        const std::string& name = unit.getName();
        if (isPlayer) {
            const uint16_t pf = game::fieldIndex(game::UF::PLAYER_FLAGS);
            uint32_t flags = pf != 0xFFFF ? e.getField(pf) : 0;
            if (guid == playerGuid && gameHandler.isAfk()) flags |= 0x2;
            text = un::playerNamePrefix(flags, "<AFK>", "<DND>", "<GM>");
            std::string titled;
            if ((nameMask & un::kPlayerPvpTitle) && !name.empty()) {
                const uint16_t t = game::fieldIndex(game::UF::PLAYER_CHOSEN_TITLE);
                if (t != 0xFFFF && e.getField(t) != 0) titled = gameHandler.getFormattedTitleFor(e.getField(t), name);
            }
            if (name.empty()) gameHandler.queryPlayerName(guid);
            text += titled.empty() ? name : titled;
            if (nameMask & un::kPlayerGuild) {
                if (const uint32_t g = gameHandler.getEntityGuildId(guid)) {
                    const std::string& gn = gameHandler.lookupGuildName(g);
                    if (!gn.empty()) text += "\n<" + gn + ">";
                }
            }
        } else {
            text = name;
            const uint16_t petNumber = game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER);
            const bool hasPetNumber = petNumber != 0xFFFF && e.getField(petNumber) != 0;
            if (!hasPetNumber) {
                const std::string sub = gameHandler.getCachedCreatureSubName(unit.getEntry());
                if (!sub.empty()) text += "\n<" + sub + ">";
            }
        }
        if (text.empty()) return;

        // 0x007e5420: a fifth of a yard tall, more for a big model, measured
        // on screen through the projection at the anchor's depth.
        float modelHeight = 0.0f;
        if (glm::vec3 base; app.getRenderPositionForGuid(guid, base)) modelHeight = anchor.z - base.z;
        const float yards = un::textHeight(modelHeight);
        const float px = yards * std::abs(proj[1][1]) * 0.5f * screenH / clipW;
        if (px < 1.0f) return;

        const ImU32 color = toU32(targetCircleColor(gameHandler, unit), 255);
        const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
        // Lines centred, the block's bottom on the anchor.
        float y = at->y - size.y;
        size_t start = 0;
        while (start <= text.size()) {
            const size_t end = text.find('\n', start);
            const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const ImVec2 ls = font->CalcTextSizeA(px, FLT_MAX, 0.0f, line.c_str());
            outlinedText(drawList, font, px, ImVec2(at->x - ls.x * 0.5f, y), color, line.c_str());
            y += ls.y;
            if (end == std::string::npos) break;
            start = end + 1;
        }

        // The raid icon over the name, a yard square, fading in from the
        // camera (0x007e5340, 0x007e52a0).
        const uint8_t mark = gameHandler.getEntityRaidMark(guid);
        if (mark < game::GameHandler::kRaidMarkCount) {
            if (VkDescriptorSet tex = getRaidTargetIcon(mark, assets)) {
                const float iconPx = std::abs(proj[1][1]) * 0.5f * screenH / clipW;
                const int alpha = un::raidIconAlpha(glm::distance(camPos, anchor));
                const float top = at->y - size.y - iconPx;
                drawList->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(at->x - iconPx * 0.5f, top),
                                   ImVec2(at->x + iconPx * 0.5f, top + iconPx), ImVec2(0, 0), ImVec2(1, 1),
                                   IM_COL32(255, 255, 255, alpha));
            }
        }
    };

    for (const auto& entityPtr : nearby) {
        if (!entityPtr || !entityPtr->isUnit()) continue;
        const uint64_t guid = entityPtr->getGuid();
        auto* unit = static_cast<game::Unit*>(entityPtr.get());
        const bool isPlayer = entityPtr->getType() == game::ObjectType::PLAYER;

        if (guid == playerGuid) {
            if (un::ownNameShown(nameMask)) drawOverheadName(guid, *entityPtr, *unit, true);
            continue;
        }

        const un::UnitFacts facts = unitFacts(gameHandler, *entityPtr, *unit);
        const bool isTarget = guid == targetGuid;

        // The plate, if this unit has one: the cvars (0x0072b060), within 41
        // yards of the player, and on screen two thirds of a yard over the
        // name (0x00715720).
        glm::vec3 unitPos;
        if (!app.getRenderPositionForGuid(guid, unitPos))
            unitPos = core::coords::canonicalToRender(glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
        std::optional<ImVec2> plateAt;
        if (un::plateShown(facts, plates) &&
            rendering::unit_name_anchor::plateInRange(playerRenderPos, unitPos)) {
            const glm::vec3 p = namePoint(guid, *entityPtr) +
                                glm::vec3(0.0f, 0.0f, rendering::unit_name_anchor::kPlateLift);
            plateAt = project(viewProj, p, screenW, screenH);
            if (plateAt && (plateAt->x < 0.0f || plateAt->x > screenW || plateAt->y < 0.0f ||
                            plateAt->y > screenH)) {
                plateAt.reset();
            }
        }

        if (!plateAt) {
            if (un::nameShown(facts, nameMask, isTarget, false)) drawOverheadName(guid, *entityPtr, *unit, isPlayer);
            continue;
        }

        // ---- The plate (0x0098f790, laid out by 0x0098f390) ----
        // With a target, every other plate at alpha 0x7f (0x0098e9f0).
        const int alpha = static_cast<int>(rendering::unit_name_anchor::plateAlpha(targetGuid != 0, isTarget) * 255.0f);
        // The frame is 0.1 by 0.025 (0x00b2da74, 0x00b2da70); its bottom
        // centre stands on the projected point.
        const float fw = 0.1f * ui, fh = 0.025f * ui;
        float left = plateAt->x - fw * 0.5f;
        float bottom = plateAt->y;
        if (!allowPlateOverlap) {
            const float top = ui::plateTopClearOf(placed, left, left + fw, bottom - fh, fh, 0.0f);
            bottom = top + fh;
            placed.push_back({.x0 = left, .y0 = bottom - fh, .x1 = left + fw, .y1 = bottom});
        }
        auto box = [&](float x, float yUp, float w, float h) {
            // Frame space: from the bottom-left, y up.
            return std::pair<ImVec2, ImVec2>(ImVec2(left + x, bottom - yUp - h), ImVec2(left + x + w, bottom - yUp));
        };

        // Health bar: 0.804 by 0.281 of the frame, 0.031 and 0.125 in from
        // its bottom-left, filled with UI-TargetingFrame-BarFill in the
        // colour 0x0098ee30 gives: a hostile player's class with
        // ShowClassColorInNameplate, else red for hostile, blue for a
        // friendly player, green for a friendly creature, yellow neutral.
        const int reaction = gameHandler.unitReactionToPlayer(*unit);
        ImU32 barColor;
        if (classColours && isPlayer && reaction <= 2 && entityClassId(entityPtr.get()) != 0) {
            barColor = classColorU32(entityClassId(entityPtr.get()), alpha);
        } else if (reaction <= 2) {
            barColor = IM_COL32(255, 0, 0, alpha);
        } else if (isPlayer) {
            barColor = IM_COL32(0, 0, 255, alpha);
        } else if (reaction >= 5) {
            barColor = IM_COL32(0, 255, 0, alpha);
        } else {
            barColor = IM_COL32(255, 255, 0, alpha);
        }
        const auto [hb0, hb1] = box(fw * 0.031f, fh * 0.125f, fw * 0.804f, fh * 0.281f);
        const float health = unit->getMaxHealth() > 0
            ? std::clamp(static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth()), 0.0f, 1.0f)
            : 0.0f;
        const ImVec2 hbFill(hb0.x + (hb1.x - hb0.x) * health, hb1.y);
        if (VkDescriptorSet fill = plateTexture(assets, "Interface\\TargetingFrame\\UI-TargetingFrame-BarFill.blp")) {
            drawList->AddImage((ImTextureID)(uintptr_t)fill, hb0, hbFill, ImVec2(0, 0), ImVec2(health, 1), barColor);
        } else {
            drawList->AddRectFilled(hb0, hbFill, barColor);
        }
        // Nameplate-Border over the whole frame.
        const auto [fr0, fr1] = box(0.0f, 0.0f, fw, fh);
        if (VkDescriptorSet border = plateTexture(assets, "Interface\\Tooltips\\Nameplate-Border.blp")) {
            drawList->AddImage((ImTextureID)(uintptr_t)border, fr0, fr1, ImVec2(0, 0), ImVec2(1, 1),
                               IM_COL32(255, 255, 255, alpha));
        } else {
            drawList->AddRect(hb0, hb1, IM_COL32(0, 0, 0, alpha));
        }

        // The name, white in NAMEPLATE_FONT at 0.01, its bottom on the
        // frame's centre; the level at 0.009 in its difficulty colour, centred
        // 0.092 in from the right and 0.284 up - or the skull for a hostile
        // unit ten levels over the player, or a boss (0x0098ef10).
        const std::string& name = unit->getName();
        if (!name.empty()) {
            const float px = 0.01f * ui;
            const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, name.c_str());
            outlinedText(drawList, font, px, ImVec2(left + fw * 0.5f - sz.x * 0.5f, bottom - fh * 0.5f - sz.y),
                         IM_COL32(255, 255, 255, alpha), name.c_str());
        } else if (isPlayer) {
            gameHandler.queryPlayerName(guid);
        }
        const ImVec2 levelCentre(left + fw - fw * 0.092f, bottom - fh * 0.284f);
        uint32_t typeFlags = 0, rank = 0;
        if (!isPlayer) {
            const auto& cache = gameHandler.getCreatureInfoCache();
            if (auto it = cache.find(unit->getEntry()); it != cache.end()) {
                typeFlags = it->second.typeFlags;
                rank = it->second.rank;
            }
        }
        const bool boss = (typeFlags & 0x4) != 0;  // 0x00715d70
        const uint32_t playerLevel = gameHandler.getPlayerLevel();
        const bool showLevel = !boss && (reaction > 2 || static_cast<int>(unit->getLevel()) - 10 <
                                                             static_cast<int>(playerLevel));
        if (showLevel) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%u", unit->getLevel());
            const float px = 0.009f * ui;
            const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, buf);
            outlinedText(drawList, font, px, ImVec2(levelCentre.x - sz.x * 0.5f, levelCentre.y - sz.y * 0.5f),
                         levelColor(unit->getLevel(), playerLevel, alpha), buf);
        } else if (VkDescriptorSet skull = plateTexture(assets, "Interface\\TargetingFrame\\UI-TargetingFrame-Skull.blp")) {
            const float s = 0.01f * ui * 0.5f;
            drawList->AddImage((ImTextureID)(uintptr_t)skull, ImVec2(levelCentre.x - s, levelCentre.y - s),
                               ImVec2(levelCentre.x + s, levelCentre.y + s), ImVec2(0, 0), ImVec2(1, 1),
                               IM_COL32(255, 255, 255, alpha));
        }
        // The elite dragon over the level for an elite, a rare elite or a
        // boss (0x0098e6e0), 0.0294 by 0.0215.
        if (boss || rank == 1 || rank == 2) {
            if (VkDescriptorSet elite = plateTexture(assets, "Interface\\Tooltips\\EliteNameplateIcon.blp")) {
                const float w = 0.0294f * ui, h = 0.0215f * ui;
                const ImVec2 c(levelCentre.x + 0.003f * ui, levelCentre.y + 0.001f * ui);
                drawList->AddImage((ImTextureID)(uintptr_t)elite, ImVec2(c.x - w * 0.5f, c.y - h * 0.5f),
                                   ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), ImVec2(0, 0), ImVec2(0.578125f, 0.84375f),
                                   IM_COL32(255, 255, 255, alpha));
            }
        }
        // The raid icon, 0.02 square, its right edge on the frame's left
        // (0x0098e740).
        const uint8_t mark = gameHandler.getEntityRaidMark(guid);
        if (mark < game::GameHandler::kRaidMarkCount) {
            if (VkDescriptorSet tex = getRaidTargetIcon(mark, assets)) {
                const float s = 0.02f * ui;
                const float cy = bottom - fh * 0.5f;
                drawList->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(left - s, cy - s * 0.5f),
                                   ImVec2(left, cy + s * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                                   IM_COL32(255, 255, 255, alpha));
            }
        }

        // The cast bar, for the target only (0x00720e50) and with
        // showVKeyCastbar: the health bar mirrored under the frame, orange
        // while casting and green while channelling, its border the plate's
        // own turned over or the shield when it cannot be interrupted
        // (0x0098f040), the spell's icon at its left.
        if (isTarget && targetCastBar) {
            const auto* cs = gameHandler.getUnitCastState(guid);
            if (cs && cs->casting && cs->timeTotal > 0.0f) {
                const float done = std::clamp((cs->timeTotal - cs->timeRemaining) / cs->timeTotal, 0.0f, 1.0f);
                const float pct = cs->isChannel ? 1.0f - done : done;
                const auto [cb0, cb1] = box(fw * 0.031f, -fh + fh * (1.0f - 0.125f - 0.281f), fw * 0.804f, fh * 0.281f);
                const ImU32 fillColor = cs->isChannel ? IM_COL32(0, 255, 0, alpha) : IM_COL32(255, 178, 0, alpha);
                const ImVec2 cbFill(cb0.x + (cb1.x - cb0.x) * pct, cb1.y);
                if (VkDescriptorSet fill = plateTexture(assets, "Interface\\TargetingFrame\\UI-TargetingFrame-BarFill.blp")) {
                    drawList->AddImage((ImTextureID)(uintptr_t)fill, cb0, cbFill, ImVec2(0, 0), ImVec2(pct, 1), fillColor);
                } else {
                    drawList->AddRectFilled(cb0, cbFill, fillColor);
                }
                const auto [cf0, cf1] = box(0.0f, -fh, fw, fh);
                const char* frame = cs->interruptible ? "Interface\\Tooltips\\Nameplate-Border.blp"
                                                      : "Interface\\Tooltips\\Nameplate-CastBar-Shield.blp";
                if (VkDescriptorSet tex = plateTexture(assets, frame)) {
                    drawList->AddImage((ImTextureID)(uintptr_t)tex, cf0, cf1,
                                       cs->interruptible ? ImVec2(0, 1) : ImVec2(0, 0),
                                       cs->interruptible ? ImVec2(1, 0) : ImVec2(1, 1),
                                       IM_COL32(255, 255, 255, alpha));
                }
                if (VkDescriptorSet icon = cs->spellId ? getSpellIcon(cs->spellId, assets) : VK_NULL_HANDLE) {
                    const float s = 0.01f * ui;
                    const float cy = (cb0.y + cb1.y) * 0.5f;
                    drawList->AddImage((ImTextureID)(uintptr_t)icon, ImVec2(cb0.x - s - 1.0f, cy - s * 0.5f),
                                       ImVec2(cb0.x - 1.0f, cy + s * 0.5f));
                }
            }
        }

        // A plate is a button: the pointer over it makes the unit the
        // mouseover, and a click targets it.
        if (!ImGui::GetIO().WantCaptureMouse) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            if (m.x >= fr0.x && m.x <= fr1.x && m.y >= fr0.y && m.y <= fr1.y) {
                gameHandler.setMouseoverGuid(guid);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gameHandler.setTarget(guid);
            }
        }
    }
}

}  // namespace ui
}  // namespace wowee
