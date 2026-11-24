// Moving about: the movement keys, turning, walking somewhere, the camera.

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "core/application.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "mcp/game_view.hpp"
#include "mcp/hands.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"

namespace wowee::mcp {

namespace {

constexpr float kDegrees = 57.29578f;

SDL_Scancode directionKey(const std::string& direction) {
    if (direction == "forward") return SDL_SCANCODE_W;
    if (direction == "back") return SDL_SCANCODE_S;
    if (direction == "strafe_left") return SDL_SCANCODE_Q;
    if (direction == "strafe_right") return SDL_SCANCODE_E;
    if (direction == "turn_left") return SDL_SCANCODE_A;
    if (direction == "turn_right") return SDL_SCANCODE_D;
    if (direction == "up") return SDL_SCANCODE_SPACE;
    if (direction == "down") return SDL_SCANCODE_X;
    return SDL_SCANCODE_UNKNOWN;
}

rendering::CameraController* cameraOf(ToolContext& context) {
    auto* renderer = context.app.getRenderer();
    return renderer ? renderer->getCameraController() : nullptr;
}

json placeNow(ToolContext& context) {
    json out = {{"pos", vec3Json(playerAt(context))}, {"facing", rounded(playerFacing(context), 4)}};
    if (auto* cc = cameraOf(context)) {
        out["speed"] = rounded(cc->getLocomotionSpeed(), 2);
        out["grounded"] = cc->isGrounded();
        if (cc->isSwimming()) out["swimming"] = true;
    }
    return out;
}

/// The end of a movement tool: where it began, where it ended, what came of it.
Script::Fn tellMoved(ToolContext& context, Aftermath after, std::shared_ptr<glm::vec3> from) {
    return finishWith(context, after, [&context, from] {
        json out = placeNow(context);
        out["moved_yards"] = rounded(glm::length(playerAt(context) - *from), 2);
        return out;
    });
}

Script::Fn noteStart(ToolContext& context, const std::shared_ptr<glm::vec3>& from) {
    return [&context, from](Script&) {
        *from = playerAt(context);
        return Script::Step::Next;
    };
}

void move(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    auto from = std::make_shared<glm::vec3>(0.0f);
    std::vector<SDL_Scancode> keys;
    for (const auto& d : args["directions"]) keys.push_back(directionKey(d.get<std::string>()));
    const double seconds = args.value("seconds", 1.0);
    const bool jump = args.value("jump", false);
    script.then(wantKeysFree(context));
    script.then(noteStart(context, from));
    script.then([&context, keys](Script&) {
        for (const auto k : keys) context.input.keyDown(k);
        return Script::Step::Next;
    });
    if (jump) {
        script.then(Script::seconds(std::min(0.3, seconds * 0.5)));
        for (auto& fn : pressKey(context, SDL_SCANCODE_SPACE, SDL_KMOD_NONE, 0.08)) script.then(std::move(fn));
    }
    script.then(Script::seconds(seconds));
    script.then([&context, keys](Script&) {
        for (const auto k : keys) context.input.keyUp(k);
        return Script::Step::Next;
    });
    script.then(Script::frames(3));
    script.then(tellMoved(context, after, from));
}

void jump(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    (void)args;
    struct Seen {
        float z = 0.0f;
        float top = -1e9f;
        bool air = false;
    };
    auto seen = std::make_shared<Seen>();
    script.then(wantKeysFree(context));
    script.then([&context, seen](Script&) {
        seen->z = seen->top = playerAt(context).z;
        context.input.keyDown(SDL_SCANCODE_SPACE);
        return Script::Step::Next;
    });
    script.then(Script::frames(2));
    script.then([&context](Script&) {
        context.input.keyUp(SDL_SCANCODE_SPACE);
        return Script::Step::Next;
    });
    script.then([&context, seen](Script& s) {
        auto* cc = cameraOf(context);
        seen->top = std::max(seen->top, playerAt(context).z);
        if (cc && !cc->isGrounded()) seen->air = true;
        if (s.elapsed() > 2.5 || (seen->air && cc && cc->isGrounded()) || (!seen->air && s.elapsed() > 0.6)) {
            return Script::Step::Next;
        }
        return Script::Step::Again;
    });
    script.then(finishWith(context, after, [&context, seen] {
        json out = placeNow(context);
        out["left_the_ground"] = seen->air;
        out["height"] = rounded(seen->top - seen->z, 2);
        return out;
    }));
}

void stop(ToolContext& context, const json& args, Script& script) {
    (void)args;
    const Aftermath after(context);
    script.then([&context](Script&) {
        context.input.releaseAll();
        return Script::Step::Next;
    });
    script.then(Script::frames(2));
    // Autorun ends with the back key, as a player ends it.
    script.then([&context](Script& s) {
        auto* cc = cameraOf(context);
        if (cc && cc->isAutoRunning()) s.first(pressKey(context, SDL_SCANCODE_S, SDL_KMOD_NONE, 0.05));
        return Script::Step::Next;
    });
    script.then(finishWith(context, after, [&context] { return placeNow(context); }));
}

/// The place a call means: "to", or an entity.
std::function<std::optional<glm::vec3>()> placeMeant(ToolContext& context, const json& args, std::string& bad) {
    if (args.contains("to")) {
        if (args["to"].size() < 2 || args["to"].size() > 3) {
            bad = "to: [x, y] or [x, y, z]";
            return {};
        }
        const float x = args["to"][0].get<float>();
        const float y = args["to"][1].get<float>();
        std::optional<float> z;
        if (args["to"].size() == 3) z = args["to"][2].get<float>();
        return [&context, x, y, z] { return std::optional<glm::vec3>(glm::vec3(x, y, z ? *z : playerAt(context).z)); };
    }
    const auto entity = findEntity(context, args);
    if (!entity) {
        bad = "no such entity in sight of the client";
        return {};
    }
    const uint64_t guid = entity->getGuid();
    return [&context, guid]() -> std::optional<glm::vec3> {
        auto* gh = context.app.getGameHandler();
        const auto e = gh ? gh->getEntityManager().getEntity(guid) : nullptr;
        if (!e) return std::nullopt;
        return entityAt(*e);
    };
}

void turn(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string method = args.value("method", std::string("auto"));
    std::string bad;
    std::function<std::optional<glm::vec3>()> target;
    if (args.contains("degrees") || args.contains("facing")) {
        const float facing = args.contains("facing")
                                 ? args["facing"].get<float>()
                                 : playerFacing(context) - args["degrees"].get<float>() / kDegrees;
        target = [&context, facing] {
            return std::optional<glm::vec3>(playerAt(context) +
                                            100.0f * glm::vec3(std::cos(facing), std::sin(facing), 0.0f));
        };
    } else {
        target = placeMeant(context, args, bad);
    }
    script.then([bad](Script& s) { return bad.empty() ? Script::Step::Next : s.fail(bad); });
    if (!bad.empty()) return;
    script.then(wantKeysFree(context));
    if (method != "mouse") {
        for (auto& fn : turnToward(context, target, false)) script.then(std::move(fn));
    }
    if (method != "keys") {
        // A frame's worth of a held key is several degrees; what is left of
        // them is the mouse's.
        for (auto& fn : turnToward(context, target, true)) script.then(std::move(fn));
    }
    script.then(finishWith(context, after, [&context, target] {
        json out = placeNow(context);
        if (const auto at = target()) {
            out["off_by_degrees"] = rounded(turnTo(playerAt(context), playerFacing(context), *at) * kDegrees, 1);
        }
        return out;
    }));
}

void walk(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    std::string bad;
    const auto goal = placeMeant(context, args, bad);
    script.then([bad](Script& s) { return bad.empty() ? Script::Step::Next : s.fail(bad); });
    if (!bad.empty()) return;
    WalkOptions options;
    options.stopWithin = static_cast<float>(args.value("stop_within", args.contains("to") ? 1.0 : 3.0));
    options.timeout = args.value("timeout", 60.0);
    options.stopInCombat = args.value("stop_in_combat", false);
    auto told = std::make_shared<json>(json::object());
    for (auto& fn : walkTo(context, goal, options, told)) script.then(std::move(fn));
    script.then(finishWith(context, after, [told] { return *told; }));
}

void camera(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const bool hasYaw = args.contains("yaw");
    const bool hasPitch = args.contains("pitch");
    if (hasYaw || hasPitch) {
        const float yaw = args.value("yaw", 0.0f);
        const float pitch = args.value("pitch", 0.0f);
        const auto left = [&context, hasYaw, hasPitch, yaw, pitch] {
            auto* cc = cameraOf(context);
            if (!cc) return glm::vec2(0.0f);
            const auto pose = cc->currentViewPose();
            return glm::vec2(hasYaw ? std::remainder(yaw - pose.yaw, 360.0f) : 0.0f,
                             hasPitch ? pitch - pose.pitch : 0.0f);
        };
        for (auto& fn : dragView(context, SDL_BUTTON_LEFT, left)) script.then(std::move(fn));
    }
    if (args.contains("distance")) {
        const float wanted = args["distance"].get<float>();
        // The wheel, over the world, until it is there or stops getting
        // nearer.
        script.then([&context](Script& s) {
            const auto p = worldPoint(context);
            if (!p) return s.fail("the interface covers the window");
            context.input.moveTo(*p);
            return Script::Step::Next;
        });
        script.then(Script::frames(3));
        script.then([&context, wanted, turns = 0, wait = 0, last = -1.0f](Script&) mutable {
            auto* cc = cameraOf(context);
            if (!cc || turns > 60) return Script::Step::Next;
            if (wait-- > 0) return Script::Step::Again;
            const float now = cc->getUserTargetDistance();
            if (std::fabs(now - wanted) < 0.6f) return Script::Step::Next;
            if (last >= 0.0f && std::fabs(now - last) < 0.01f) return Script::Step::Next;  // at a limit
            if (last >= 0.0f && (now - wanted) * (last - wanted) < 0.0f) return Script::Step::Next;
            last = now;
            context.input.wheel(now > wanted ? 1.0f : -1.0f);
            ++turns;
            wait = 2;
            return Script::Step::Again;
        });
        script.then(Script::seconds(0.5));
    }
    script.then(Script::frames(3));
    script.then(finishWith(context, after, [&context] {
        auto* cc = cameraOf(context);
        if (!cc) return json::object();
        const auto pose = cc->currentViewPose();
        return json{{"distance", rounded(pose.distance, 2)},
                    {"pitch", rounded(pose.pitch, 1)},
                    {"yaw_from_facing", rounded(pose.yaw, 1)},
                    {"facing", rounded(playerFacing(context), 4)}};
    }));
}

}  // namespace

void addMoveTools(Toolbox& box) {
    using namespace schema;
    const json place = list("x, y or x, y, z in .gps coordinates", json{{"type", "number"}}, 3);
    Tool t = handsTool(
        "move",
        "Hold movement keys for a while: forward, back, strafe_left, strafe_right, turn_left, turn_right, up and down "
        "(swimming and flying).",
        object({required("directions",
                         list("which, together",
                              choice("", {"forward", "back", "strafe_left", "strafe_right", "turn_left", "turn_right",
                                          "up", "down"}),
                              4)),
                optional("seconds", number("1 by default", 0.05, 30)), optional("jump", flag("jump on the way"))}),
        move);
    t.patienceOf = [](const json& a) { return a.value("seconds", 1.0) + 10.0; };
    box.push_back(std::move(t));
    box.push_back(handsTool("jump", "Press the jump key once and watch the jump.", object({}), jump));
    box.push_back(handsTool("stop", "Let go of everything held, and end autorun.", object({}), stop));
    box.push_back(handsTool(
        "turn", "Turn the character: by degrees (clockwise), to a facing, or toward a place or an entity.",
        object({optional("degrees", number("clockwise; negative is left", -360, 360)),
                optional("facing", number("radians, 0 north, as .go has it", -7, 7)), optional("to", place),
                optional("guid", guid("0x...")), optional("name", text("the nearest with this in its name", 64)),
                optional("unit", text("target, pet, ...", 16)),
                optional("method", choice("the turn keys, a drag with the right button, or the one and then the "
                                          "other (auto, the default)",
                                          {"auto", "keys", "mouse"}))}),
        turn));
    Tool w = handsTool(
        "walk_to",
        "Walk (or swim) to a place or an entity with the movement keys, round what is in the way. Says how far it got "
        "and where it was stuck.",
        object({optional("to", place), optional("guid", guid("0x...")),
                optional("name", text("the nearest with this in its name", 64)),
                optional("unit", text("target, pet, ...", 16)),
                optional("stop_within", number("yards; 1 for a place, 3 for an entity by default", 0.3, 50)),
                optional("timeout", number("seconds, 60 by default", 1, 300)),
                optional("stop_in_combat", flag("end the walk when combat begins"))}),
        walk);
    w.patienceOf = [](const json& a) { return a.value("timeout", 60.0) + 10.0; };
    box.push_back(std::move(w));
    box.push_back(handsTool(
        "camera",
        "Turn the camera by dragging with the left button and zoom with the wheel: yaw from the facing (0 behind the "
        "character), pitch (looking down is positive, as /gps has it) in degrees, distance in yards.",
        object({optional("yaw", number("degrees", -360, 360)), optional("pitch", number("degrees", -89, 89)),
                optional("distance", number("yards", 0, 50))}),
        camera));
}

}  // namespace wowee::mcp
