// The overview: where the client is, who and where the player is, the camera,
// the clock and what was last refused.

#include <cmath>
#include <utility>

#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/window.hpp"
#include "game/character.hpp"
#include "game/game_handler.hpp"
#include "game/zone_area.hpp"
#include "mcp/game_view.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "ui/ui_manager.hpp"

namespace wowee::mcp {

Tool lookTool(std::string name, std::string description, json schema,
              std::function<json(ToolContext&, const json&)> look, Tool::Where where) {
    Tool tool;
    tool.info = {.name = std::move(name),
                 .description = std::move(description),
                 .inputSchema = std::move(schema),
                 .readOnly = true};
    tool.where = where;
    tool.build = [look = std::move(look)](ToolContext& context, const json& args, Script& script) {
        script.then([&context, args, look](Script& s) {
            try {
                return s.done(look(context, args));
            } catch (const ToolError& e) {
                return s.fail(e.what());
            }
        });
    };
    return tool;
}

Tool handsTool(std::string name, std::string description, json schema,
               std::function<void(ToolContext&, const json&, Script&)> build, double patience, Tool::Where where) {
    Tool tool;
    tool.info = {.name = std::move(name),
                 .description = std::move(description),
                 .inputSchema = std::move(schema),
                 .readOnly = false};
    tool.where = where;
    tool.hands = true;
    tool.patience = patience;
    tool.build = std::move(build);
    return tool;
}

namespace {

json movementFlagNames(uint32_t flags) {
    using F = game::MovementFlags;
    json out = json::array();
    const auto add = [&](F f, const char* name) {
        if (flags & static_cast<uint32_t>(f)) out.push_back(name);
    };
    add(F::FORWARD, "forward");
    add(F::BACKWARD, "backward");
    add(F::STRAFE_LEFT, "strafe_left");
    add(F::STRAFE_RIGHT, "strafe_right");
    add(F::TURN_LEFT, "turn_left");
    add(F::TURN_RIGHT, "turn_right");
    add(F::WALKING, "walking");
    add(F::ONTRANSPORT, "on_transport");
    add(F::LEVITATING, "levitating");
    add(F::ROOT, "rooted");
    add(F::FALLING, "falling");
    add(F::FALLINGFAR, "falling_far");
    add(F::SWIMMING, "swimming");
    add(F::ASCENDING, "ascending");
    add(F::DESCENDING, "descending");
    add(F::CAN_FLY, "can_fly");
    add(F::FLYING, "flying");
    add(F::WATER_WALK, "water_walk");
    add(F::FEATHER_FALL, "feather_fall");
    add(F::HOVER, "hover");
    return out;
}

/// What the interface says of the player, with the calls its unit frames make.
const char* const kPlayerChunk = R"lua(
local out = {}
out.name = UnitName("player")
out.race = UnitRace("player")
out.class = UnitClass("player")
out.level = UnitLevel("player")
out.hp = {UnitHealth("player"), UnitHealthMax("player")}
local _, power = UnitPowerType("player")
out.power = {UnitPower("player"), UnitPowerMax("player")}
out.power_type = power
out.xp = {UnitXP("player"), UnitXPMax("player")}
out.money = GetMoney()
out.combat = UnitAffectingCombat("player") and true or false
out.mounted = IsMounted() and true or false
out.swimming = IsSwimming() and true or false
out.flying = IsFlying() and true or false
out.falling = IsFalling() and true or false
out.stealthed = IsStealthed() and true or false
out.resting = IsResting() and true or false
out.indoors = IsIndoors() and true or false
out.pvp = UnitIsPVP("player") and true or false
out.afk = UnitIsAFK("player") and true or false
local name, _, _, _, _, ends = UnitCastingInfo("player")
if name then out.casting = {spell = name, left = ends / 1000 - GetTime()} end
name, _, _, _, _, ends = UnitChannelInfo("player")
if name then out.channeling = {spell = name, left = ends / 1000 - GetTime()} end
out.auras = {}
for _, kind in ipairs({"HELPFUL", "HARMFUL"}) do
  for i = 1, 40 do
    local aura, _, _, count, _, _, expires, _, _, _, id = UnitAura("player", i, kind)
    if not aura then break end
    out.auras[#out.auras + 1] = {name = aura, id = id, stacks = count > 1 and count or nil,
      harmful = kind == "HARMFUL" or nil, left = expires and expires > 0 and expires - GetTime() or nil}
  end
end
if UnitExists("target") then
  out.target = {name = UnitName("target"), guid = UnitGUID("target"), level = UnitLevel("target"),
    hp = {UnitHealth("target"), UnitHealthMax("target")}, dead = UnitIsDead("target") and true or nil,
    reaction = UnitReaction("target", "player")}
end
local mirror = {}
for i = 1, 3 do
  local timer, value, max, scale, paused, label = GetMirrorTimerInfo(i)
  if timer and timer ~= "UNKNOWN" then
    mirror[#mirror + 1] = {name = timer, left = GetMirrorTimerProgress(timer) / 1000, max = max / 1000}
  end
end
if #mirror > 0 then out.timers = mirror end
return out
)lua";

json stateOf(ToolContext& context, const json& args) {
    (void)args;
    auto& app = context.app;
    json out = {{"client", clientState(context)},
                {"frame", context.frame},
                {"fps", context.frameSeconds > 0.0 ? rounded(1.0 / context.frameSeconds, 1) : 0.0}};
    if (auto* window = app.getWindow()) {
        out["window"] = json::array({window->getWidth(), window->getHeight()});
    }
    auto* gh = app.getGameHandler();
    if (app.getState() == core::AppState::CHARACTER_SELECTION && gh) {
        json characters = json::array();
        for (const auto& c : gh->getCharacters()) {
            characters.push_back({{"name", c.name},
                                  {"level", c.level},
                                  {"race", game::getRaceName(c.race)},
                                  {"class", game::getClassName(c.characterClass)},
                                  {"map", c.mapId}});
        }
        out["characters"] = std::move(characters);
        out["hint"] = "enter_world presses Enter for the selected character";
        return out;
    }
    if (!inWorld(context)) return out;

    auto* renderer = app.getRenderer();
    const auto& mi = gh->getMovementInfo();
    json place = {{"map", gh->getCurrentMapId()},
                  {"pos", vec3Json(playerAt(context))},
                  {"facing", rounded(playerFacing(context), 4)}};
    if (const auto* texts = renderer ? renderer->getZoneTexts() : nullptr) {
        place["zone"] = texts->zone;
        place["subzone"] = texts->subzone;
        place["minimap"] = texts->minimap;
    }
    const glm::vec3 me = playerAt(context);
    char go[128];
    std::snprintf(go, sizeof(go), ".go xyz %.3f %.3f %.3f %u %.4f", me.x, me.y, me.z, gh->getCurrentMapId(),
                  playerFacing(context));
    place["go"] = go;
    // The liquid the character is in, not whatever lies over its room.
    if (renderer) {
        if (const auto h = rendering::liquid_query::surfaceOverUnit(renderer->liquidWorld(), me))
            place["water_over_feet"] = rounded(*h - me.z, 2);
    }
    out["place"] = std::move(place);

    json movement = {{"flags", movementFlagNames(mi.flags)}};
    if (auto* cc = renderer ? renderer->getCameraController() : nullptr) {
        movement["speed"] = rounded(cc->getLocomotionSpeed(), 2);
        movement["grounded"] = cc->isGrounded();
        movement["swimming"] = cc->isSwimming();
        movement["airborne_s"] = rounded(cc->airborneSeconds(), 2);
        movement["autorun"] = cc->isAutoRunning();
        movement["sitting"] = cc->isSitting();
        const auto pose = cc->currentViewPose();
        out["camera"] = {{"distance", rounded(pose.distance, 2)},
                         {"pitch", rounded(pose.pitch, 1)},
                         {"yaw_from_facing", rounded(pose.yaw, 1)},
                         {"first_person", cc->isFirstPersonView()}};
        if (const auto* camera = renderer->getCamera()) out["camera"]["pos"] = vec3Json(camera->getPosition());
    }
    out["movement"] = std::move(movement);

    json world = {{"weather", gh->getWeatherType()}, {"weather_intensity", rounded(gh->getWeatherIntensity(), 2)}};
    if (auto* lighting = renderer ? renderer->getLightingManager() : nullptr) {
        world["hour"] = rounded(lighting->getTimeOfDayHours(), 2);
    }
    if (renderer) world["indoors"] = renderer->isPlayerIndoors();
    out["world"] = std::move(world);

    out["player"] = tryQueryLua(context, kPlayerChunk);

    json errors = json::array();
    for (const auto& e : context.log.after(0, 5, [](const LogLine& l) {
             return l.level >= static_cast<int>(core::kLogLevelError);
         })) {
        errors.push_back({{"seq", e.seq}, {"text", e.value.text.substr(0, 200)}});
    }
    out["recent_errors"] = std::move(errors);
    out["cursors"] = {{"log", context.log.last()},
                      {"events", context.events.last()},
                      {"chat", context.chat.last()},
                      {"combat_log", context.combat.last()},
                      {"refused", context.refusals.last()}};
    return out;
}

json serverInfo(ToolContext& context, const json& args) {
    json refused = json::array();
    const auto since = args.value("refused_since", static_cast<uint64_t>(0));
    for (const auto& e : context.refusals.after(since, 20, [](const RefusedLine&) { return true; })) {
        refused.push_back({{"seq", e.seq}, {"what", e.value.what}, {"why", e.value.why},
                           {"ago_s", rounded(context.now - e.value.at, 1)}});
    }
    return {{"options",
             {{"lua", context.options.lua},
              {"allow", socialList(context.options.allow)},
              {"not_allowed", socialList(kSocialAll & ~context.options.allow)}}},
            {"uptime_s", rounded(context.now, 0)},
            {"last_act_was_the_servers", context.origin.isServer()},
            {"refused", std::move(refused)},
            {"refused_last", context.refusals.last()}};
}

}  // namespace

void addStateTools(Toolbox& box) {
    box.push_back(lookTool("state",
                           "Overview of the client and the player: login / char select / in world / dead, position "
                           "(.gps coordinates), facing, zone, speed and movement flags, camera, clock and weather, "
                           "health, power, casting, auras, target, fps, recent errors, and the cursors of the logs.",
                           schema::object({}), stateOf, Tool::Where::Anywhere));
    box.push_back(lookTool("server_info",
                           "What this server was started to allow, and what it has refused (packets and acts towards "
                           "other players).",
                           schema::object({schema::optional("refused_since",
                                                            schema::integer("only refusals after this seq", 0,
                                                                            9007199254740991LL))}),
                           serverInfo, Tool::Where::Anywhere));
}

}  // namespace wowee::mcp
