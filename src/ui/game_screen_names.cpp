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

#include "addons/addon_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/lua_engine.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "game/game_handler.hpp"
#include "game/unit_name_rules.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/camera.hpp"
#include "rendering/imgui_blend.hpp"
#include "rendering/renderer.hpp"
#include "rendering/unit_name_anchor.hpp"
#include "rendering/wmo_renderer.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/nameplate_cast_bar.hpp"
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
/// and PvP rules, for anything else not flagged out of reach and not friendly.
bool mayAttack(game::GameHandler& gh, const game::Unit& unit, bool isPlayer) {
    if (isPlayer) return playerMayAttackPlayer(gh, unit);
    constexpr uint32_t kNonAttackable = 0x00000002, kNotAttackable1 = 0x00000080,
                       kImmuneToPlayers = 0x00000100;
    if (unit.getUnitFlags() & (kNonAttackable | kNotAttackable1 | kImmuneToPlayers |
                               game::UNIT_FLAG_NOT_SELECTABLE)) {
        return false;
    }
    return unit.isHostile() || gh.unitReactionToPlayer(unit) <= 4;
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

/// One of the interface's global strings (FrameScript_GetText, 0x00819d40),
/// empty without an interface or such a string.
std::string interfaceText(const char* name) {
    auto* addons = core::Application::getInstance().getAddonManager();
    auto* engine = addons ? addons->getLuaEngine() : nullptr;
    return engine && engine->isInitialized() ? engine->globalText(name) : std::string{};
}

/// 0x0061e830: the "<Owner's Pet>" line under a unit something made, empty
/// for none.
std::string summonTitleLine(game::GameHandler& gh, const game::Entity& e, const game::Unit& unit) {
    auto ownerOf = [](const game::Entity& x) {
        const uint64_t charmedBy = guidAt(x, game::UF::UNIT_FIELD_CHARMEDBY);
        return charmedBy != 0 ? charmedBy : guidAt(x, game::UF::UNIT_FIELD_CREATEDBY);
    };
    uint64_t owner = ownerOf(e);
    if (owner == 0) return {};
    if (auto o = gh.getEntityManager().getEntity(owner)) {
        if (const uint64_t up = ownerOf(*o)) owner = up;
    }
    // The creating spell's first SUMMON effect's SummonProperties Title.
    std::optional<int32_t> propertiesTitle;
    const uint16_t bySpell = game::fieldIndex(game::UF::UNIT_CREATED_BY_SPELL);
    if (const uint32_t spellId = bySpell != 0xFFFF ? e.getField(bySpell) : 0) {
        gh.getSpellName(spellId);  // fills the cache
        auto it = gh.spellNameCacheRef().find(spellId);
        for (int k = 0; it != gh.spellNameCacheRef().end() && k < 3; ++k) {
            if (it->second.effectIds[k] != un::kEffectSummon) continue;
            auto* am = core::Application::getInstance().getAssetManager();
            auto props = am ? am->loadDBCOptional("SummonProperties.dbc") : nullptr;
            const int32_t row = props && props->getFieldCount() > 3 ? props->findRecordById(it->second.effectMiscValuesB[k]) : -1;
            if (row >= 0) propertiesTitle = static_cast<int32_t>(props->getUInt32(static_cast<uint32_t>(row), 3));
            break;
        }
    }
    const int title = un::summonTitle(propertiesTitle, gh.getCreatureType(unit.getEntry()));
    if (title == 0) return {};
    const std::string& ownerName = gh.lookupName(owner);
    if (ownerName.empty()) {
        if (isPlayerGuid(owner)) gh.queryPlayerName(owner);
        return {};
    }
    return un::summonTitleText(title, ownerName, interfaceText(un::summonTitleKey(title).c_str()));
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
            // Another server's player, as the name query named a realm.
            if (guid != playerGuid && !gameHandler.getCachedPlayerRealm(guid).empty()) {
                const std::string label = interfaceText("FOREIGN_SERVER_LABEL");
                text += label.empty() ? un::kForeignServerLabelEnUS : label;
            }
        } else {
            text = name;
            const uint16_t petNumber = game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER);
            const bool hasPetNumber = petNumber != 0xFFFF && e.getField(petNumber) != 0;
            if (!hasPetNumber) {
                const std::string sub = gameHandler.getCachedCreatureSubName(unit.getEntry());
                if (!sub.empty()) text += "\n<" + sub + ">";
            }
            if (const std::string title = summonTitleLine(gameHandler, e, unit); !title.empty())
                text += "\n<" + title + ">";
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

    // The plates this frame, drawn after every name so the glow can go on
    // the nearest under the pointer.
    struct PlateRec {
        const game::Entity* entity = nullptr;
        const game::Unit* unit = nullptr;
        uint64_t guid = 0;
        bool isPlayer = false;
        bool isTarget = false;
        float left = 0.0f, bottom = 0.0f;
        float depth = 0.0f;
    };
    static thread_local std::vector<PlateRec> plateRecs;
    plateRecs.clear();
    struct PlateHealth {
        uint32_t health = 0;
        bool seen = false;
        double hurtUntil = 0.0;
        uint64_t frame = 0;
    };
    static std::unordered_map<uint64_t, PlateHealth> plateHealth;
    static uint64_t frameNo = 0;
    ++frameNo;
    const double now = ImGui::GetTime();
    const float fw = 0.1f * ui, fh = 0.025f * ui;

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

        // The frame is 0.1 by 0.025 (0x00b2da74, 0x00b2da70); its bottom
        // centre stands on the projected point.
        float left = plateAt->x - fw * 0.5f;
        float bottom = plateAt->y;
        if (!allowPlateOverlap) {
            const float top = ui::plateTopClearOf(placed, left, left + fw, bottom - fh, fh, 0.0f);
            bottom = top + fh;
            placed.push_back({.x0 = left, .y0 = bottom - fh, .x1 = left + fw, .y1 = bottom});
        }
        // A unit hurt since the last frame has its plate's name red for five
        // seconds (0x0073f330 -> 0x0098e5b0).
        auto& hp = plateHealth[guid];
        if (hp.seen && unit->getHealth() < hp.health) hp.hurtUntil = now + un::kPlateHurtSeconds;
        hp.health = unit->getHealth();
        hp.seen = true;
        hp.frame = frameNo;
        float clipW = 1.0f;
        project(viewProj, unitPos, screenW, screenH, &clipW);
        plateRecs.push_back({.entity = entityPtr.get(), .unit = unit, .guid = guid, .isPlayer = isPlayer,
                             .isTarget = isTarget, .left = left, .bottom = bottom, .depth = clipW});
    }

    // Forget the units whose plates are gone.
    for (auto it = plateHealth.begin(); it != plateHealth.end();) {
        it = it->second.frame != frameNo ? plateHealth.erase(it) : std::next(it);
    }

    // 0x007271d0: the glow goes on the nearest plate under the pointer that
    // is not the target's, while no button is held.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool pointerFree = !ImGui::GetIO().WantCaptureMouse && !ImGui::IsAnyMouseDown();
    const PlateRec* glowing = nullptr;
    for (const PlateRec& r : plateRecs) {
        if (r.isTarget || !pointerFree) continue;
        if (mouse.x < r.left || mouse.x > r.left + fw || mouse.y < r.bottom - fh || mouse.y > r.bottom) continue;
        if (!glowing || r.depth < glowing->depth) glowing = &r;
    }

    // The threat flash, by threatWarning (0x00519df0).
    bool inDungeon = false;
    {
        static uint32_t mapSeen = 0xFFFFFFFFu;
        static bool mapDungeon = false;
        const uint32_t mapId = gameHandler.getCurrentMapId();
        if (mapId != mapSeen) {
            mapSeen = mapId;
            mapDungeon = false;
            auto map = assets ? assets->loadDBCOptional("Map.dbc") : nullptr;
            const int32_t row = map && map->getFieldCount() > 2 ? map->findRecordById(mapId) : -1;
            if (row >= 0) {
                const uint32_t type = map->getUInt32(static_cast<uint32_t>(row), 2);
                mapDungeon = type == 1 || type == 2;
            }
        }
        inDungeon = mapDungeon;
    }
    int threatMode = 3;
    try {
        threatMode = std::stoi(addons::storedCVarValue("threatWarning", "3"));
    } catch (...) {
    }
    const bool threatFlash = un::threatWarningOn(threatMode, inDungeon, gameHandler.isInGroup());

    // The target's cast bar (0x00720e50 -> 0x0098f040, run on by
    // 0x0098e9f0). While the unit casts, the bar is handed the cast; once
    // the cast is gone it runs to its end by itself and fades out green. A
    // failure or interrupt hides it, a channel stopped by an update of 0 is
    // handed over at its end and fades out red.
    static ui::PlateCastBar castBar;
    static uint64_t castBarGuid = 0;
    static uint32_t castBarEndSerial = 0;
    {
        const game::UnitCastEnd* end = targetGuid ? gameHandler.getUnitCastEnd(targetGuid) : nullptr;
        if (targetGuid != castBarGuid) {
            castBar.hide();
            castBarGuid = targetGuid;
            castBarEndSerial = end ? end->serial : 0;
        }
        const auto* cs = targetGuid ? gameHandler.getUnitCastState(targetGuid) : nullptr;
        if (!targetCastBar || !targetGuid) {
            castBar.hide();
        } else if (cs && cs->casting && cs->timeTotal > 0.0f) {
            // Attributes 0x20, a trade skill, has no bar.
            const auto attrs = cs->spellId ? gameHandler.getSpellAttributes(cs->spellId) : std::nullopt;
            const bool spellShown = attrs && (*attrs & 0x20u) == 0;
            const float v = cs->isChannel ? cs->timeRemaining : cs->timeTotal - cs->timeRemaining;
            castBar.set(0.0f, cs->timeTotal, v, cs->spellId, spellShown, cs->isChannel, !cs->interruptible);
        } else if (end && end->serial != castBarEndSerial) {
            if (end->channelZeroed && castBar.shown && castBar.channel)
                castBar.set(castBar.min, castBar.max, castBar.min, castBar.spellId, true, true,
                            castBar.notInterruptible);
            else
                castBar.hide();
        } else {
            castBar.update(ImGui::GetIO().DeltaTime);
        }
        if (end) castBarEndSerial = end->serial;
    }

    // ---- The plates (0x0098f790, laid out by 0x0098f390, each frame
    // 0x0098e9f0); the target's on top, at frame level 20 to the others' 10.
    auto drawPlate = [&](const PlateRec& r) {
        const game::Unit* unit = r.unit;
        const uint64_t guid = r.guid;
        const bool isPlayer = r.isPlayer;
        const float left = r.left, bottom = r.bottom;
        // With a target, every other plate at alpha 0x7f (0x0098e9f0).
        const int alpha =
            static_cast<int>(rendering::unit_name_anchor::plateAlpha(targetGuid != 0, r.isTarget) * 255.0f);
        auto box = [&](float x, float yUp, float w, float h) {
            // Frame space: from the bottom-left, y up.
            return std::pair<ImVec2, ImVec2>(ImVec2(left + x, bottom - yUp - h), ImVec2(left + x + w, bottom - yUp));
        };
        auto argb = [&](uint32_t c) {
            const int a = static_cast<int>(((c >> 24) & 0xFF) * alpha / 255);
            return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, a);
        };
        // Plate text: NAMEPLATE_FONT with a black shadow 0.001 right and
        // down, no outline (0x0098f790).
        const float shadowPx = std::max(1.0f, 0.001f * ui);
        auto shadowedText = [&](float px, ImVec2 at, ImU32 color, const char* text) {
            drawList->AddText(font, px, ImVec2(at.x + shadowPx, at.y + shadowPx),
                              IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) & 0xFF), text);
            drawList->AddText(font, px, at, color, text);
        };

        // BACKGROUND: the threat flash, 0.11 by 0.029, its top 0.0065 under
        // the frame's and 0.001 left of centre, UI-TargetingFrame-Flash's
        // (0, 0.53)-(0.555, 0.6), tinted by the player's threat status on
        // the unit (0x007374c0).
        if (threatFlash) {
            int status = 0;
            if (const auto* list = gameHandler.getThreatList(guid); list && !list->empty()) {
                for (size_t i = 0; i < list->size(); ++i) {
                    if ((*list)[i].victimGuid != playerGuid) continue;
                    if (i != 0) {
                        status = 1;
                    } else {
                        const uint32_t next = list->size() > 1 ? (*list)[1].threat : 0;
                        status = list->size() > 1 && next * 11 >= (*list)[0].threat * 10 ? 2 : 3;
                    }
                    break;
                }
            }
            if (const uint32_t c = un::plateThreatColor(status)) {
                if (VkDescriptorSet flash = plateTexture(assets, "Interface\\TargetingFrame\\UI-TargetingFrame-Flash.blp")) {
                    const float w = 0.11f * ui, h = 0.029f * ui;
                    const float cx = left + fw * 0.5f - 0.001f * ui;
                    const float top = bottom - fh + 0.0065f * ui;
                    drawList->AddImage((ImTextureID)(uintptr_t)flash, ImVec2(cx - w * 0.5f, top),
                                       ImVec2(cx + w * 0.5f, top + h), ImVec2(0.0f, 0.53f), ImVec2(0.555f, 0.6f),
                                       argb(c));
                }
            }
        }

        // Health bar: 0.804 by 0.281 of the frame, 0.031 and 0.125 in from
        // its bottom-left, filled with UI-TargetingFrame-BarFill in the
        // colour 0x0098ee30 gives: a hostile player's class with
        // ShowClassColorInNameplate, else red for hostile, blue for a
        // friendly player, green for a friendly creature, yellow neutral.
        const int reaction = gameHandler.unitReactionToPlayer(*unit);
        ImU32 barColor;
        if (classColours && isPlayer && reaction <= 2 && entityClassId(r.entity) != 0) {
            barColor = classColorU32(entityClassId(r.entity), alpha);
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

        // The name in NAMEPLATE_FONT at 0.01, its bottom on the frame's
        // centre: red while hurt, yellow under the glow, else white; the
        // level at 0.009 in its difficulty colour, centred 0.092 in from the
        // right and 0.284 up - or the skull for a hostile unit ten levels
        // over the player, or a boss (0x0098ef10).
        const std::string& name = unit->getName();
        if (!name.empty()) {
            const float px = 0.01f * ui;
            const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, name.c_str());
            const auto hp = plateHealth.find(guid);
            const bool hurt = hp != plateHealth.end() && now < hp->second.hurtUntil;
            shadowedText(px, ImVec2(left + fw * 0.5f - sz.x * 0.5f, bottom - fh * 0.5f - sz.y),
                         argb(un::plateNameColor(hurt, &r == glowing)), name.c_str());
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
            shadowedText(px, ImVec2(levelCentre.x - sz.x * 0.5f, levelCentre.y - sz.y * 0.5f),
                         levelColor(unit->getLevel(), playerLevel, alpha), buf);
        } else if (VkDescriptorSet skull = plateTexture(assets, "Interface\\TargetingFrame\\UI-TargetingFrame-Skull.blp")) {
            const float sh = 0.01f * ui * 0.5f;
            drawList->AddImage((ImTextureID)(uintptr_t)skull, ImVec2(levelCentre.x - sh, levelCentre.y - sh),
                               ImVec2(levelCentre.x + sh, levelCentre.y + sh), ImVec2(0, 0), ImVec2(1, 1),
                               IM_COL32(255, 255, 255, alpha));
        }
        // The elite dragon over the level for an elite, a rare elite or a
        // boss (0x0098e6e0), 0.0294 by 0.0215, 0.003 right and 0.001 down.
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
                const float sq = 0.02f * ui;
                const float cy = bottom - fh * 0.5f;
                drawList->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(left - sq, cy - sq * 0.5f),
                                   ImVec2(left, cy + sq * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                                   IM_COL32(255, 255, 255, alpha));
            }
        }

        // The cast bar, for the target only (0x00720e50) and with
        // showVKeyCastbar (0x0098f040). Its border is the plate's
        // own mirrored left to right, centred on the frame's bottom edge
        // (0x0098f390); for a cast that cannot be interrupted the shield,
        // a frame's height under it, in its place. The bar, 0.804 by 0.281
        // of the frame, hangs from the border's bottom-right 0.003125 in and
        // 0.003125 up - down for the shield; the spell's icon, 0.01 square,
        // is centred 0.0092 in from the border's bottom-left and 0.0071 up
        // (0 for the shield).
        if (r.isTarget && castBar.shown) {
            const auto& bar = castBar;
            const int barAlpha = alpha * bar.alpha / 255;
            const float pct = std::clamp(bar.filled(), 0.0f, 1.0f);
            const float borderUp = -fh * 0.5f;  // the border's bottom, from the frame's
            const float inset = 0.003125f * ui;
            const bool shield = bar.notInterruptible;
            const float barUp = !shield ? borderUp + inset : borderUp - inset;
            const auto [cb0, cb1] = box(fw - inset - fw * 0.804f, barUp, fw * 0.804f, fh * 0.281f);
            // Orange while it runs, green at its end, red when handed over
            // already there (0x0098f040, 0x0098e9f0).
            const ImU32 fillColor = bar.colour == ui::PlateCastBar::Colour::Green ? IM_COL32(0, 255, 0, barAlpha)
                                    : bar.colour == ui::PlateCastBar::Colour::Red ? IM_COL32(255, 0, 0, barAlpha)
                                                                                  : IM_COL32(255, 178, 0, barAlpha);
            const ImVec2 cbFill(cb0.x + (cb1.x - cb0.x) * pct, cb1.y);
            if (VkDescriptorSet fill = plateTexture(assets, "Interface\\TargetingFrame\\UI-TargetingFrame-BarFill.blp")) {
                drawList->AddImage((ImTextureID)(uintptr_t)fill, cb0, cbFill, ImVec2(0, 0), ImVec2(pct, 1), fillColor);
            } else {
                drawList->AddRectFilled(cb0, cbFill, fillColor);
            }
            if (!shield) {
                if (VkDescriptorSet tex = plateTexture(assets, "Interface\\Tooltips\\Nameplate-Border.blp")) {
                    const auto [cf0, cf1] = box(0.0f, borderUp, fw, fh);
                    drawList->AddImage((ImTextureID)(uintptr_t)tex, cf0, cf1, ImVec2(1, 0), ImVec2(0, 1),
                                       IM_COL32(255, 255, 255, barAlpha));
                }
            } else if (VkDescriptorSet tex = plateTexture(assets, "Interface\\Tooltips\\Nameplate-CastBar-Shield.blp")) {
                const auto [cf0, cf1] = box(0.0f, borderUp - fh * 0.5f, fw, fh);
                drawList->AddImage((ImTextureID)(uintptr_t)tex, cf0, cf1, ImVec2(0, 0), ImVec2(1, 1),
                                   IM_COL32(255, 255, 255, barAlpha));
            }
            if (VkDescriptorSet icon = bar.spellId ? getSpellIcon(bar.spellId, assets) : VK_NULL_HANDLE) {
                const float sq = 0.01f * ui;
                const float cx = left + 0.0092f * ui;
                const float cy = bottom - borderUp - (!shield ? 0.0071f * ui : 0.0f);
                drawList->AddImage((ImTextureID)(uintptr_t)icon, ImVec2(cx - sq * 0.5f, cy - sq * 0.5f),
                                   ImVec2(cx + sq * 0.5f, cy + sq * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                                   IM_COL32(255, 255, 255, barAlpha));
            }
        }

        // HIGHLIGHT: Nameplate-Glow over the whole frame on the glowing
        // plate (0x0098e910), added (blend mode 3).
        if (&r == glowing) {
            if (VkDescriptorSet glow = plateTexture(assets, "Interface\\Tooltips\\Nameplate-Glow.blp")) {
                rendering::beginAdditive(drawList);
                drawList->AddImage((ImTextureID)(uintptr_t)glow, fr0, fr1, ImVec2(0, 0), ImVec2(1, 1),
                                   IM_COL32(255, 255, 255, alpha));
                rendering::endAdditive(drawList);
            }
        }

        // A plate is a button: the pointer over it makes the unit the
        // mouseover, and a click targets it.
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (mouse.x >= fr0.x && mouse.x <= fr1.x && mouse.y >= fr0.y && mouse.y <= fr1.y) {
                gameHandler.setMouseoverGuid(guid);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gameHandler.setTarget(guid);
            }
        }
    };
    for (const PlateRec& r : plateRecs)
        if (!r.isTarget) drawPlate(r);
    for (const PlateRec& r : plateRecs)
        if (r.isTarget) drawPlate(r);
}

}  // namespace ui
}  // namespace wowee
