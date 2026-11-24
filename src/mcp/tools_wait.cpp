// Waiting for something to have happened.

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "core/application.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "mcp/game_view.hpp"
#include "mcp/hands.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "mcp/ui_view.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"
#include "ui/widget_tree.hpp"

namespace wowee::mcp {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Whether a condition holds now. Throws ToolError for one that cannot be
/// asked.
bool holds(ToolContext& context, const json& args, const Aftermath& since) {
    if (args.contains("event")) {
        const std::string name = args["event"].get<std::string>();
        return since.heard(context, {name.c_str()});
    }
    if (args.contains("log")) {
        const std::string part = lower(args["log"].get<std::string>());
        return !context.log.after(since.log, 1, [&](const LogLine& l) {
                        return lower(l.text).find(part) != std::string::npos;
                    }).empty();
    }
    if (args.contains("chat")) {
        const std::string part = lower(args["chat"].get<std::string>());
        return !context.chat.after(since.chat, 1, [&](const EventLine& l) {
                        return !l.args.empty() && lower(l.args[0]).find(part) != std::string::npos;
                    }).empty();
    }
    if (args.contains("client")) {
        const std::string now = clientState(context);
        const std::string wanted = args["client"].get<std::string>();
        if (wanted == "in_game") return now == "in_world" || now == "dead" || now == "ghost";
        return now == wanted;
    }
    if (args.contains("window")) {
        const auto* w = widgetNamed(context, args["window"].get<std::string>());
        return w && w->visible;
    }
    if (args.contains("window_closed")) {
        const auto* w = widgetNamed(context, args["window_closed"].get<std::string>());
        return !w || !w->visible;
    }
    if (args.contains("text")) {
        auto* tree = widgetTree(context);
        const std::string part = lower(args["text"].get<std::string>());
        if (!tree) return false;
        for (uint32_t id = tree->nextMaybeVisible(1); id != 0; id = tree->nextMaybeVisible(id + 1)) {
            const auto* w = tree->get(id);
            if (w && w->visible && !w->text.empty() && lower(plainText(w->text)).find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    if (args.contains("until")) {
        const std::string until = args["until"].get<std::string>();
        auto* gh = context.app.getGameHandler();
        auto* renderer = context.app.getRenderer();
        auto* cc = renderer ? renderer->getCameraController() : nullptr;
        if (!gh) return false;
        if (until == "loaded") return inWorld(context) && !context.app.loadingScreenUp();
        const auto target = gh->getEntityManager().getEntity(gh->getTargetGuid());
        const auto targetDead = [&] {
            return target && target->getType() != game::ObjectType::GAMEOBJECT &&
                   static_cast<const game::Unit&>(*target).getHealth() == 0;
        };
        if (until == "target_dead") return targetDead();
        if (until == "no_target") return gh->getTargetGuid() == 0;
        if (until == "in_combat") return gh->isInCombat();
        if (until == "out_of_combat") return tryQueryLua(context, "return not UnitAffectingCombat('player')") == true;
        if (until == "not_casting") {
            return tryQueryLua(context, "return not UnitCastingInfo('player') and not UnitChannelInfo('player')") ==
                   true;
        }
        if (until == "standing") return cc && cc->getLocomotionSpeed() < 0.05f && cc->isGrounded();
        if (until == "grounded") return cc && cc->isGrounded();
        if (until == "swimming") return cc && cc->isSwimming();
        if (until == "mounted") return gh->isMounted();
        if (until == "dismounted") return !gh->isMounted();
        if (until == "full_health") {
            return tryQueryLua(context, "return UnitHealth('player') >= UnitHealthMax('player')") == true;
        }
    }
    return false;
}

void wait(ToolContext& context, const json& args, Script& script) {
    const Aftermath since(context);
    const double timeout = args.value("timeout", 10.0);
    const bool timeOnly = args.size() - (args.contains("timeout") ? 1u : 0u) == 0;
    auto result = std::make_shared<json>(json::object());
    // Asked a few times a second: some of these are asked in Lua.
    script.then([&context, args, since, timeout, timeOnly, result, next = 0.0](Script& s) mutable {
        if (s.now() >= next) {
            next = s.now() + 0.1;
            if (!timeOnly && holds(context, args, since)) {
                *result = {{"came", true}, {"after_s", rounded(s.elapsed(), 2)}};
                return Script::Step::Next;
            }
        }
        if (s.elapsed() < timeout) return Script::Step::Again;
        *result = {{"came", timeOnly}, {"after_s", rounded(s.elapsed(), 2)}};
        return Script::Step::Next;
    });
    script.then(finishWith(context, since, [result] { return *result; }));
}

}  // namespace

void addWaitTools(Toolbox& box) {
    using namespace schema;
    Tool t;
    t.info = {
        .name = "wait",
        .description = "Wait for one thing, or for the time given: an event of the interface, a frame shown or gone, a "
                       "text on the screen, a chat line, a state of the client or the character (`loaded`: in the "
                       "world, and the loading screen gone).",
        .inputSchema = object(
            {optional("timeout", number("seconds at most, 10 by default", 0.05, 300)),
             optional("event", text("an event's name", 64)), optional("window", text("a frame that is to be shown", 96)),
             optional("window_closed", text("a frame that is to be gone", 96)),
             optional("text", text("part of a text on the screen", 96)),
             optional("chat", text("part of a chat line", 96)), optional("log", text("part of a line of the log", 96)),
             optional("client", choice("in_game: in_world, dead or ghost",
                                       {"login", "char_select", "entering_world", "in_world", "dead", "ghost",
                                        "in_game"})),
             optional("until", choice("", {"target_dead", "no_target", "in_combat", "out_of_combat", "not_casting",
                                           "standing", "grounded", "swimming", "mounted", "dismounted",
                                           "full_health", "loaded"}))}),
        .readOnly = true};
    t.where = Tool::Where::Anywhere;
    t.patienceOf = [](const json& a) { return a.value("timeout", 10.0) + 10.0; };
    t.build = wait;
    box.push_back(std::move(t));
}

}  // namespace wowee::mcp
