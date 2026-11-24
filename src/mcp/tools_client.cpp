// The client itself: into the world, out of the game, and - only where it was
// started for it - Lua.

#include <memory>
#include <string>

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "game/game_handler.hpp"
#include "mcp/game_view.hpp"
#include "mcp/hands.hpp"
#include "mcp/lua_error.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"

namespace wowee::mcp {

namespace {

void enterWorld(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const double timeout = args.value("timeout", 90.0);
    script.then([&context](Script& s) {
        const std::string state = clientState(context);
        if (state != "char_select") return s.fail("at " + state + ", not at the character selection");
        return Script::Step::Next;
    });
    // Enter goes in with the selected character (character_screen.cpp).
    for (auto& fn : pressKey(context, SDL_SCANCODE_RETURN, SDL_KMOD_NONE, 0.1)) script.then(std::move(fn));
    script.then([&context, timeout](Script& s) {
        if (inWorld(context)) return Script::Step::Next;
        return s.elapsed() > timeout ? s.fail("not in the world after " + std::to_string(static_cast<int>(timeout)) +
                                              " s; at " + clientState(context))
                                     : Script::Step::Again;
    });
    script.then(Script::seconds(2.0));
    script.then(worldShown(context, timeout));
    script.then(finishWith(context, after, [&context] {
        json out = {{"client", clientState(context)}, {"pos", vec3Json(playerAt(context))}};
        if (auto* gh = context.app.getGameHandler()) {
            if (const auto* c = gh->getActiveCharacter()) out["character"] = c->name;
        }
        return out;
    }));
}

void quit(ToolContext& context, const json& args, Script& script) {
    const bool now = args.value("now", false);
    script.then([&context, now](Script& s) {
        context.quitting = true;
        auto* gh = context.app.getGameHandler();
        auto* window = context.app.getWindow();
        if (inWorld(context) && gh && !now) {
            // As Exit Game does: logged out by the realm first.
            gh->requestLogout(true);
        } else if (window) {
            window->setShouldClose(true);
        }
        return s.done({{"quitting", true}, {"logout_first", !now}});
    });
}

json runLua(ToolContext& context, const json& args) {
    auto* manager = context.app.getAddonManager();
    auto* engine = manager ? manager->getLuaEngine() : nullptr;
    if (!engine || !engine->isInitialized()) throw ToolError("the interface is not loaded");
    const std::string code = args["code"].get<std::string>();
    if (args.value("as_macro", false)) {
        // As /run has it: insecure code, and errors to the interface.
        const uint64_t before = context.log.last();
        const bool ran = manager->runScript(code);
        json out = {{"ran", ran}};
        if (json errors = luaErrorsSince(context, before, 10); !errors.empty()) out["errors"] = std::move(errors);
        return out;
    }
    json value;
    std::string error;
    const bool ok = engine->query(
        code,
        [&value](lua_State* L, int index) {
            int budget = 4000;
            value = luaToJson(L, index, 0, budget);
        },
        error, true);
    if (!ok) throw ToolError(json{{"error", luaErrorJson(parseLuaError(error))}});
    return {{"value", std::move(value)}};
}

}  // namespace

void addClientTools(Toolbox& box) {
    using namespace schema;
    Tool e = handsTool("enter_world",
                       "At the character selection: press Enter for the selected character and wait for the world.",
                       object({optional("timeout", number("seconds, 90 by default", 5, 280))}), enterWorld, 100.0,
                       Tool::Where::Anywhere);
    e.patienceOf = [](const json& a) { return a.value("timeout", 90.0) + 15.0; };
    box.push_back(std::move(e));
    box.push_back(handsTool("quit", "End the client: logged out first, or at once.",
                            object({optional("now", flag("do not wait for the realm's logout"))}), quit, 10.0,
                            Tool::Where::Anywhere));
    Tool lua = lookTool("lua",
                        "Run Lua in the interface (the server was started with --mcp-lua): a chunk whose one value is "
                        "returned, or, as_macro, as /run does. An error comes in parts: source, line, message, stack.",
                        object({required("code", text("a chunk; `return ...` for a value", 8000)),
                                optional("as_macro", flag("as /run"))}),
                        runLua);
    lua.info.readOnly = false;
    lua.hands = true;
    box.push_back(std::move(lua));
}

}  // namespace wowee::mcp
