#include "mcp/game_view.hpp"

#include "mcp/entity_pick.hpp"

#include <limits>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "addons/lua_api_helpers.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/protocol_constants.hpp"
#include "game/quest_giver_status.hpp"
#include "game/transport_manager.hpp"
#include "mcp/tool_context.hpp"
#include "mcp/world_probe.hpp"
#include "rendering/camera.hpp"
#include "rendering/renderer.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/scene_pick.hpp"

namespace wowee::mcp {

namespace {

constexpr double kDegrees = 57.29577951308232;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* typeName(game::ObjectType type) {
    switch (type) {
        case game::ObjectType::UNIT: return "unit";
        case game::ObjectType::PLAYER: return "player";
        case game::ObjectType::GAMEOBJECT: return "object";
        case game::ObjectType::CORPSE: return "corpse";
        case game::ObjectType::DYNAMICOBJECT: return "dynamic";
        default: return "other";
    }
}

/// UnitReaction's eight, 1 to 8.
const char* reactionName(int reaction) {
    constexpr std::array<const char*, 9> kNames = {"unknown",  "hated",   "hostile", "unfriendly", "neutral",
                                                   "friendly", "honored", "revered", "exalted"};
    return reaction >= 1 && reaction <= 8 ? kNames[static_cast<size_t>(reaction)] : kNames[0];
}

json npcRoles(uint32_t flags) {
    json out = json::array();
    const auto add = [&](uint32_t bit, const char* name) {
        if (flags & bit) out.push_back(name);
    };
    add(game::NPC_FLAG_GOSSIP, "gossip");
    add(game::NPC_FLAG_QUESTGIVER, "questgiver");
    add(game::NPC_FLAG_TRAINER, "trainer");
    add(game::NPC_FLAG_ANY_VENDOR, "vendor");
    add(game::NPC_FLAG_REPAIR, "repair");
    add(game::NPC_FLAG_FLIGHT_MASTER, "flightmaster");
    add(game::NPC_FLAG_SPIRIT_HEALER, "spirithealer");
    add(game::NPC_FLAG_SPIRIT_GUIDE, "spiritguide");
    add(game::NPC_FLAG_INNKEEPER, "innkeeper");
    add(game::NPC_FLAG_BANKER, "banker");
    add(game::NPC_FLAG_AUCTIONEER, "auctioneer");
    add(game::NPC_FLAG_GUILD_BANKER, "guildbanker");
    add(game::NPC_FLAG_POSTMASTER, "mailbox");
    return out;
}

/// The mark over a quest giver's head, as the client was told it.
const char* questMark(game::QuestGiverStatus status) {
    using S = game::QuestGiverStatus;
    switch (status) {
        case S::AVAILABLE:
        case S::AVAILABLE_REP: return "!";
        case S::LOW_LEVEL_AVAILABLE:
        case S::LOW_LEVEL_AVAILABLE_REP: return "! (low level)";
        case S::INCOMPLETE: return "? (unfinished)";
        case S::REWARD:
        case S::REWARD2:
        case S::REWARD_REP:
        case S::LOW_LEVEL_REWARD_REP: return "?";
        default: return nullptr;
    }
}

const char* objectKind(uint32_t type) {
    constexpr std::array<const char*, 36> kNames = {
        "door", "button", "questgiver", "chest", "binder", "generic", "trap", "chair", "spell_focus", "text",
        "goober", "transport", "area_damage", "camera", "map_object", "mo_transport", "duel_arbiter", "fishing_bobber",
        "ritual", "mailbox", "guard_post", "spellcaster", "meeting_stone", "flag_stand", "fishing_hole", "flag_drop",
        "mini_game", "lottery", "capture_point", "aura_generator", "dungeon_difficulty", "barber_chair",
        "destructible", "guild_bank", "trapdoor", "?"};
    return kNames[std::min<size_t>(type, kNames.size() - 1)];
}

}  // namespace

std::string guidText(uint64_t guid) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(guid));
    return buffer;
}

uint64_t guidFromText(const std::string& text) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return 0;
    return std::strtoull(text.c_str() + 2, nullptr, 16);
}

double rounded(double v, int places) {
    const double f = std::pow(10.0, places);
    return std::isfinite(v) ? std::round(v * f) / f : 0.0;
}

json vec3Json(const glm::vec3& v) {
    return json::array({rounded(v.x, 2), rounded(v.y, 2), rounded(v.z, 2)});
}

glm::vec3 playerAt(ToolContext& context) {
    // The renderer's axes are .gps's.
    auto* renderer = context.app.getRenderer();
    return renderer ? renderer->getCharacterPosition() : glm::vec3(0.0f);
}

float playerFacing(ToolContext& context) {
    // The renderer's, which the movement sent takes over a frame later.
    auto* renderer = context.app.getRenderer();
    if (!renderer) return 0.0f;
    return core::coords::canonicalToPlayerFacing(
        core::coords::characterYawDegToCanonical(renderer->getCharacterYaw()));
}

namespace {

/// The transport an entity is, null where it is none.
const game::ActiveTransport* transportOf(ToolContext& context, const game::Entity& entity) {
    if (entity.getType() != game::ObjectType::GAMEOBJECT) return nullptr;
    auto* gh = context.app.getGameHandler();
    auto* manager = gh ? gh->getTransportManager() : nullptr;
    return manager ? manager->getTransport(entity.getGuid()) : nullptr;
}

}  // namespace

glm::vec3 entityAt(ToolContext& context, const game::Entity& entity) {
    if (const auto* transport = transportOf(context, entity)) {
        return core::coords::canonicalToServer(transport->position);
    }
    return core::coords::canonicalToServer(glm::vec3(entity.getX(), entity.getY(), entity.getZ()));
}

json faceOwnerJson(ToolContext& context, rendering::collision::Facet::Kind kind, uint32_t owner, const glm::vec3& at) {
    using Kind = rendering::collision::Facet::Kind;
    json out = {{"of", probe::kindName(kind)}};
    if (kind != Kind::Building && kind != Kind::Doodad) return out;
    const uint64_t guid = context.app.gameObjectOfFace(kind == Kind::Building, owner, at);
    if (guid == 0) return out;
    out["of"] = "object";
    out["guid"] = guidText(guid);
    auto* gh = context.app.getGameHandler();
    if (const auto e = gh ? gh->getEntityManager().getEntity(guid) : nullptr) out["name"] = entityName(context, *e);
    return out;
}

float turnTo(const glm::vec3& from, float facing, const glm::vec3& at) {
    return core::coords::normalizeAngleRad(std::atan2(at.y - from.y, at.x - from.x) - facing);
}

double bearingTo(ToolContext& context, const glm::vec3& at) {
    return -static_cast<double>(turnTo(playerAt(context), playerFacing(context), at)) * kDegrees;
}

bool inWorld(ToolContext& context) {
    auto* gh = context.app.getGameHandler();
    return context.app.getState() == core::AppState::IN_GAME && gh &&
           gh->getState() == game::WorldState::IN_WORLD;
}

std::string clientState(ToolContext& context) {
    auto* gh = context.app.getGameHandler();
    switch (context.app.getState()) {
        case core::AppState::AUTHENTICATION: return "login";
        case core::AppState::REALM_SELECTION: return "realm_select";
        case core::AppState::CHARACTER_CREATION: return "char_create";
        case core::AppState::CHARACTER_SELECTION: return "char_select";
        case core::AppState::DISCONNECTED: return "disconnected";
        case core::AppState::FIRST_RUN:
        case core::AppState::DATA_FOLDER: return "setup";
        case core::AppState::IN_GAME: break;
    }
    if (!gh || gh->getState() != game::WorldState::IN_WORLD) return "entering_world";
    if (gh->isPlayerGhost()) return "ghost";
    if (gh->isPlayerDead()) return "dead";
    return "in_world";
}

bool screenIsTheUsers(ToolContext& context) {
    switch (context.app.getState()) {
        case core::AppState::AUTHENTICATION:
        case core::AppState::REALM_SELECTION:
        case core::AppState::CHARACTER_CREATION:
        case core::AppState::DISCONNECTED:
        case core::AppState::FIRST_RUN:
        case core::AppState::DATA_FOLDER: return true;
        case core::AppState::CHARACTER_SELECTION:
        case core::AppState::IN_GAME: break;
    }
    return false;
}

std::optional<glm::vec2> screenAt(ToolContext& context, const glm::vec3& at) {
    auto* renderer = context.app.getRenderer();
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    auto* window = context.app.getWindow();
    if (!camera || !window) return std::nullopt;
    const glm::vec4 clip = camera->getViewProjectionMatrix() * glm::vec4(at, 1.0f);
    if (clip.w <= 0.01f) return std::nullopt;
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return glm::vec2((ndc.x + 1.0f) * 0.5f * static_cast<float>(window->getWidth()),
                     (ndc.y + 1.0f) * 0.5f * static_cast<float>(window->getHeight()));
}

bool interfaceAt(ToolContext& context, const glm::vec2& pixel) {
    (void)context;
    return ui::frameXmlPointOwnsMouse(pixel.x, pixel.y);
}

uint64_t pickedAt(ToolContext& context, const glm::vec2& pixel) {
    auto* renderer = context.app.getRenderer();
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    auto* window = context.app.getWindow();
    auto* gh = context.app.getGameHandler();
    if (!camera || !window || !gh) return 0;
    const rendering::Ray ray =
        camera->screenToWorldRay(pixel.x, pixel.y, static_cast<float>(window->getWidth()),
                                 static_cast<float>(window->getHeight()));
    return ui::pickScene(*gh, ray, ui::ScenePickParams{}).resolve();
}

std::optional<glm::vec2> clickPointOf(ToolContext& context, uint64_t guid, uint64_t* insteadPicked) {
    auto* gh = context.app.getGameHandler();
    auto* window = context.app.getWindow();
    if (insteadPicked) *insteadPicked = 0;
    if (!gh || !window) return std::nullopt;
    const auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) return std::nullopt;
    glm::vec3 centre = entityAt(context, *entity);
    float radius = 1.0f;
    if (!context.app.getRenderBoundsForGuid(guid, centre, radius)) centre.z += 1.0f;
    radius = std::clamp(radius, 0.2f, 4.0f);
    // The middle first, then about it: what stands in front may hide the one
    // and not the other.
    constexpr std::array<glm::vec3, 11> kOffsets = {{{0.0f, 0.0f, 0.0f},
                                                     {0.0f, 0.0f, 0.4f},
                                                     {0.0f, 0.0f, -0.4f},
                                                     {0.0f, 0.0f, 0.7f},
                                                     {0.4f, 0.0f, 0.0f},
                                                     {-0.4f, 0.0f, 0.0f},
                                                     {0.0f, 0.4f, 0.0f},
                                                     {0.0f, -0.4f, 0.0f},
                                                     {0.0f, 0.0f, -0.7f},
                                                     {0.3f, 0.3f, 0.3f},
                                                     {-0.3f, -0.3f, 0.3f}}};
    const auto w = static_cast<float>(window->getWidth());
    const auto h = static_cast<float>(window->getHeight());
    for (const auto& o : kOffsets) {
        const auto at = screenAt(context, centre + o * radius);
        if (!at || at->x < 2.0f || at->y < 2.0f || at->x > w - 2.0f || at->y > h - 2.0f) continue;
        if (interfaceAt(context, *at)) continue;
        const uint64_t picked = pickedAt(context, *at);
        if (picked == guid) return at;
        if (insteadPicked && picked != 0 && *insteadPicked == 0) *insteadPicked = picked;
    }
    return std::nullopt;
}

std::string entityName(ToolContext& context, const game::Entity& entity) {
    auto* gh = context.app.getGameHandler();
    switch (entity.getType()) {
        case game::ObjectType::PLAYER:
        case game::ObjectType::UNIT: {
            const auto& u = static_cast<const game::Unit&>(entity);
            if (!u.getName().empty()) return u.getName();
            return gh ? gh->lookupName(entity.getGuid()) : std::string();
        }
        case game::ObjectType::GAMEOBJECT: {
            const auto& g = static_cast<const game::GameObject&>(entity);
            if (!g.getName().empty()) return g.getName();
            if (gh) {
                if (const auto* info = gh->getCachedGameObjectInfo(g.getEntry())) return info->name;
            }
            return {};
        }
        default:
            return {};
    }
}

bool inLineOfSight(ToolContext& context, const game::Entity& entity) {
    auto* renderer = context.app.getRenderer();
    if (!renderer) return false;
    glm::vec3 centre = entityAt(context, entity);
    float radius = 0.0f;
    if (!context.app.getRenderBoundsForGuid(entity.getGuid(), centre, radius)) centre.z += 1.0f;
    const glm::vec3 eyes = playerAt(context) + glm::vec3(0.0f, 0.0f, 1.8f);
    const auto hit = renderer->intersectSegment(eyes, centre);
    // What it stands on or is built of may be met just before its middle.
    const float length = glm::length(centre - eyes);
    return !hit || (1.0f - *hit) * length <= std::max(radius, 0.5f);
}

json entityJson(ToolContext& context, const game::Entity& entity, bool detail) {
    auto* gh = context.app.getGameHandler();
    const glm::vec3 at = entityAt(context, entity);
    const glm::vec3 me = playerAt(context);
    json out = {{"guid", guidText(entity.getGuid())},
                {"type", typeName(entity.getType())},
                {"name", entityName(context, entity)},
                {"distance", rounded(glm::length(at - me), 1)},
                {"bearing", rounded(bearingTo(context, at), 0)},
                {"pos", vec3Json(at)}};
    if (!gh) return out;
    if (gh->getTargetGuid() == entity.getGuid()) out["target"] = true;

    if (entity.getType() == game::ObjectType::UNIT || entity.getType() == game::ObjectType::PLAYER) {
        const auto& u = static_cast<const game::Unit&>(entity);
        const bool self = entity.getGuid() == gh->getPlayerGuid();
        const bool dead = u.getMaxHealth() > 0 && u.getHealth() == 0;
        out["level"] = u.getLevel();
        out["hp"] = json::array({u.getHealth(), u.getMaxHealth()});
        if (dead) out["dead"] = true;
        if (!self) {
            out["reaction"] = reactionName(gh->unitReactionToPlayer(u));
            if (!dead && gh->unitMayAttack(gh->getPlayerGuid(), entity.getGuid())) out["attackable"] = true;
        }
        if (u.getUnitFlags() & game::UNIT_FLAG_IN_COMBAT) out["combat"] = true;
        if (u.getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) out["selectable"] = false;
        if (u.getNpcFlags() != 0) out["roles"] = npcRoles(u.getNpcFlags());
        if (u.getNpcFlags() & game::NPC_FLAG_QUESTGIVER) {
            if (const char* mark = questMark(gh->getQuestGiverStatus(entity.getGuid()))) out["quest"] = mark;
        }
        if (dead && (u.getDynamicFlags() & game::UNIT_DYNFLAG_LOOTABLE)) out["lootable"] = true;
        if (const uint64_t its = game::unitTargetGuid(entity); its != 0) {
            out["its_target"] = its == gh->getPlayerGuid() ? std::string("player") : guidText(its);
        }
        if (const auto* cast = gh->getUnitCastState(entity.getGuid()); cast && cast->casting) {
            out["casting"] = {{"spell", gh->getSpellName(cast->spellId)},
                              {"id", cast->spellId},
                              {"left", rounded(cast->timeRemaining, 1)}};
        }
        if (u.getMountDisplayId() != 0) out["mounted"] = true;
        if (detail) {
            out["entry"] = u.getEntry();
            out["display"] = u.getDisplayId();
            out["power"] = json::array({u.getPower(), u.getMaxPower()});
            out["power_type"] = u.getPowerType();
            out["faction"] = u.getFactionTemplate();
            out["unit_flags"] = guidText(u.getUnitFlags());
            out["npc_flags"] = guidText(u.getNpcFlags());
            out["dynamic_flags"] = guidText(u.getDynamicFlags());
            out["stand_state"] = u.getStandState();
            out["combat_reach"] = rounded(u.getCombatReach(), 2);
            out["move_flags"] = guidText(entity.getReportedMoveFlags());
            out["speed"] = rounded(entity.getMoveSpeed(), 2);
            if (const uint64_t owner = game::unitSummonedByGuid(entity); owner != 0) out["owner"] = guidText(owner);
            json auras = json::array();
            if (const auto* slots = gh->getUnitAuras(entity.getGuid())) {
                for (const auto& a : *slots) {
                    if (a.isEmpty()) continue;
                    auras.push_back({{"id", a.spellId}, {"name", gh->getSpellName(a.spellId)}, {"stacks", a.charges}});
                }
            }
            out["auras"] = std::move(auras);
        }
    } else if (entity.getType() == game::ObjectType::GAMEOBJECT) {
        const auto& g = static_cast<const game::GameObject&>(entity);
        const auto pointer = gh->gameObjectPointer(entity.getGuid());
        out["usable"] = pointer.interactive;
        if (const auto* info = gh->getCachedGameObjectInfo(g.getEntry())) out["kind"] = objectKind(info->type);
        if (const auto* transport = transportOf(context, entity)) {
            json t = {{"spawned_at", vec3Json(core::coords::canonicalToServer(
                                         glm::vec3(entity.getX(), entity.getY(), entity.getZ())))}};
            if (!transport->onThisMap) t["on_another_map"] = true;
            if (gh->getMovementInfo().transportGuid == entity.getGuid()) t["player_aboard"] = true;
            out["transport"] = std::move(t);
        }
        if (detail) {
            out["entry"] = g.getEntry();
            out["display"] = g.getDisplayId();
            out["pointer"] = pointer.tracked;
            out["draws_nothing"] = context.app.gameObjectDrawsNothing(entity.getGuid());
        }
    }
    if (detail) {
        out["facing"] = rounded(core::coords::canonicalToPlayerFacing(entity.getOrientation()), 4);
        out["line_of_sight"] = inLineOfSight(context, entity);
        if (const auto screen = screenAt(context, at)) {
            out["screen"] = json::array({rounded(screen->x, 0), rounded(screen->y, 0)});
        }
        uint64_t instead = 0;
        if (const auto click = clickPointOf(context, entity.getGuid(), &instead)) {
            out["click_at"] = json::array({rounded(click->x, 0), rounded(click->y, 0)});
        } else if (instead != 0) {
            out["hidden_by"] = guidText(instead);
        }
    }
    return out;
}

std::shared_ptr<game::Entity> findEntity(ToolContext& context, const json& args) {
    auto* gh = context.app.getGameHandler();
    if (!gh) return nullptr;
    const auto& manager = gh->getEntityManager();
    if (args.contains("guid")) return manager.getEntity(guidFromText(args["guid"].get<std::string>()));
    if (args.contains("unit")) {
        return manager.getEntity(addons::resolveUnitGuid(gh, lower(args["unit"].get<std::string>())));
    }
    if (!args.contains("name")) return nullptr;
    const std::string wanted = lower(args["name"].get<std::string>());
    if (wanted.empty()) return nullptr;
    const glm::vec3 me = playerAt(context);
    std::shared_ptr<game::Entity> best;
    float bestRank = std::numeric_limits<float>::infinity();
    const std::string state = args.value("state", std::string());
    const bool livingFirst = !args.value("nearest", false);
    for (const auto& [guid, entity] : manager.getEntities()) {
        if (!entity || guid == gh->getPlayerGuid()) continue;
        const std::string name = lower(entityName(context, *entity));
        if (name.empty() || name.find(wanted) == std::string::npos) continue;
        PickCandidate c;
        c.wholeName = name == wanted;
        c.distance = glm::length(entityAt(context, *entity) - me);
        if (entity->getType() == game::ObjectType::UNIT) {
            const auto& u = static_cast<const game::Unit&>(*entity);
            c.unit = true;
            c.dead = u.getHealth() == 0;
            c.lootable = c.dead && (u.getDynamicFlags() & game::UNIT_DYNFLAG_LOOTABLE) != 0;
        }
        if (const float rank = pickRank(c, state, livingFirst); rank < bestRank) {
            bestRank = rank;
            best = entity;
        }
    }
    return best;
}

}  // namespace wowee::mcp
