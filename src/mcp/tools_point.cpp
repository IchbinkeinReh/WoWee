// The mouse: on the interface's frames, and on what stands in the world.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

#include "core/application.hpp"
#include "core/window.hpp"
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

int buttonNamed(const std::string& name) {
    if (name == "right") return SDL_BUTTON_RIGHT;
    if (name == "middle") return SDL_BUTTON_MIDDLE;
    return SDL_BUTTON_LEFT;
}

SDL_Keymod modsOf(const json& args) {
    SDL_Keymod mods = SDL_KMOD_NONE;
    if (!args.contains("modifiers")) return mods;
    for (const auto& m : args["modifiers"]) {
        if (m == "shift") mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_LSHIFT);
        if (m == "ctrl") mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_LCTRL);
        if (m == "alt") mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_LALT);
    }
    return mods;
}

glm::vec2 pointOf(const json& args, const char* x, const char* y) {
    return {args[x].get<float>(), args[y].get<float>()};
}

/// Where on a frame to click now, or why not.
std::optional<glm::vec2> clickPointNow(ToolContext& context, const std::string& name, Script& s) {
    const auto* w = widgetNamed(context, name);
    if (!w) {
        s.failed = true;
        s.result = "no frame named " + name;
        return std::nullopt;
    }
    if (!w->visible) {
        s.failed = true;
        s.result = name + " is not shown";
        return std::nullopt;
    }
    std::string covering;
    const auto at = clickPointOf(context, *w, &covering);
    if (!at) {
        s.failed = true;
        s.result = name + " cannot be clicked: " + (covering.empty() ? "off the screen" : covering + " is over it");
    }
    return at;
}

void clickUi(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string name = args["name"].get<std::string>();
    const int button = buttonNamed(args.value("button", std::string("left")));
    const int clicks = args.value("clicks", 1);
    auto landed = std::make_shared<std::string>();
    Script* self = &script;
    for (auto& fn : movePointer(context, [&context, name, self] { return clickPointNow(context, name, *self); })) {
        script.then(std::move(fn));
    }
    // What has the pointer now is what will be clicked.
    script.then([&context, landed](Script&) {
        if (const auto* w = widgetAt(context, context.input.pointer())) *landed = widgetLabel(*w);
        return Script::Step::Next;
    });
    for (auto& fn : pressButton(context, button, modsOf(args), clicks)) script.then(std::move(fn));
    script.then(Script::seconds(args.value("settle", 0.25)));
    script.then(finishWith(context, after, [&context, name, landed] {
        json out = {{"clicked", name}, {"landed_on", *landed}};
        const auto* w = widgetNamed(context, name);
        out["still_shown"] = w && w->visible;
        if (w && w->objectType == "CheckButton") out["checked"] = w->checked;
        json windows = json::array();
        for (const auto* open : openWindows(context)) windows.push_back(widgetLabel(*open));
        out["windows"] = std::move(windows);
        return out;
    }));
}

void hover(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    Script* self = &script;
    std::function<std::optional<glm::vec2>()> where;
    if (args.contains("name")) {
        const std::string name = args["name"].get<std::string>();
        where = [&context, name, self] { return clickPointNow(context, name, *self); };
    } else if (args.contains("guid")) {
        const uint64_t guid = guidFromText(args["guid"].get<std::string>());
        where = [&context, guid] { return clickPointOf(context, guid); };
    } else if (args.contains("x") && args.contains("y")) {
        const glm::vec2 p = pointOf(args, "x", "y");
        where = [p] { return std::optional<glm::vec2>(p); };
    } else {
        script.then([](Script& s) { return s.fail("name, guid, or x and y"); });
        return;
    }
    for (auto& fn : movePointer(context, where)) script.then(std::move(fn));
    script.then(Script::seconds(args.value("seconds", 0.3)));
    script.then(finishWith(context, after, [&context] {
        json out;
        const glm::vec2 p = context.input.pointer();
        out["pointer"] = json::array({rounded(p.x, 0), rounded(p.y, 0)});
        if (const auto* w = widgetAt(context, p)) out["under"] = widgetLabel(*w);
        else if (const uint64_t picked = pickedAt(context, p)) out["under"] = guidText(picked);
        if (json lines = tooltipLines(context, "GameTooltip"); !lines.is_null()) out["tooltip"] = std::move(lines);
        const json mouseover = tryQueryLua(context, "return UnitExists('mouseover') and UnitName('mouseover') or nil");
        if (!mouseover.is_null()) out["mouseover"] = mouseover;
        return out;
    }));
}

void mouse(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    if (screenIsTheUsers(context)) {
        const std::string where = clientState(context);
        script.then([where](Script& s) { return s.fail("no clicks on the screen " + where); });
        return;
    }
    const std::string action = args["action"].get<std::string>();
    const int button = buttonNamed(args.value("button", std::string("left")));
    const bool hasPoint = args.contains("x") && args.contains("y");
    if (action != "wheel" && action != "up" && !hasPoint) {
        script.then([](Script& s) { return s.fail("x and y are wanted"); });
        return;
    }
    if (hasPoint && action != "drag") {
        const glm::vec2 p = pointOf(args, "x", "y");
        for (auto& fn : movePointer(context, [p] { return std::optional<glm::vec2>(p); })) script.then(std::move(fn));
    }
    if (action == "click") {
        for (auto& fn : pressButton(context, button, modsOf(args), args.value("clicks", 1))) script.then(std::move(fn));
    } else if (action == "down" || action == "up") {
        const bool down = action == "down";
        script.then([&context, button, down](Script&) {
            context.input.button(button, down);
            return Script::Step::Next;
        });
    } else if (action == "wheel") {
        const float turns = args.value("wheel", 1.0f);
        for (int i = 0; i < static_cast<int>(std::ceil(std::fabs(turns))); ++i) {
            script.then([&context, turns](Script&) {
                context.input.wheel(turns > 0.0f ? 1.0f : -1.0f);
                return Script::Step::Next;
            });
            script.then(Script::frames(2));
        }
    } else if (action == "drag") {
        if (!args.contains("to_x") || !args.contains("to_y")) {
            script.then([](Script& s) { return s.fail("to_x and to_y are wanted"); });
            return;
        }
        for (auto& fn : dragFromTo(context, pointOf(args, "x", "y"), pointOf(args, "to_x", "to_y"), button)) {
            script.then(std::move(fn));
        }
    } else if (action == "release") {
        script.then([&context](Script&) {
            context.input.releasePointer();
            return Script::Step::Next;
        });
    }
    script.then(Script::seconds(0.2));
    script.then(finishWith(context, after, [&context] {
        json out;
        const glm::vec2 p = context.input.pointer();
        out["pointer"] = json::array({rounded(p.x, 0), rounded(p.y, 0)});
        if (const auto* w = widgetAt(context, p)) out["under"] = widgetLabel(*w);
        else if (const uint64_t picked = pickedAt(context, p)) out["under"] = guidText(picked);
        return out;
    }));
}

/// Steps that bring an entity into the window, so that it can be pointed at:
/// the character turned to it, and the camera moved where something is in the
/// way of it.
Steps bringIntoView(ToolContext& context, uint64_t guid) {
    Steps out;
    out.push_back([&context, guid](Script& s) {
        auto* gh = context.app.getGameHandler();
        const auto e = gh ? gh->getEntityManager().getEntity(guid) : nullptr;
        if (!e) return s.fail("it is gone");
        if (clickPointOf(context, guid)) return Script::Step::Next;
        // The camera is behind the character; what it faces is in view.
        Steps steps = turnToward(
            context,
            [&context, guid]() -> std::optional<glm::vec3> {
                auto* game = context.app.getGameHandler();
                const auto it = game ? game->getEntityManager().getEntity(guid) : nullptr;
                if (!it) return std::nullopt;
                return entityAt(*it);
            },
            false);
        steps.push_back(Script::seconds(0.6));
        s.first(std::move(steps));
        return Script::Step::Next;
    });
    // Behind the character itself, or under it: looked at from elsewhere.
    struct Views {
        static Script::Fn make(ToolContext& context, uint64_t guid, size_t i) {
            return [&context, guid, i](Script& s) {
                constexpr std::array<glm::vec2, 4> kViews = {{{0.0f, 30.0f}, {45.0f, 25.0f}, {-45.0f, 25.0f},
                                                              {0.0f, 55.0f}}};
                if (i >= kViews.size() || clickPointOf(context, guid)) return Script::Step::Next;
                const glm::vec2 view = kViews[i];
                Steps steps = dragView(context, SDL_BUTTON_LEFT, [&context, view] {
                    auto* renderer = context.app.getRenderer();
                    auto* cc = renderer ? renderer->getCameraController() : nullptr;
                    if (!cc) return glm::vec2(0.0f);
                    const auto pose = cc->currentViewPose();
                    return glm::vec2(std::remainder(view.x - pose.yaw, 360.0f), view.y - pose.pitch);
                });
                steps.push_back(Script::seconds(0.3));
                steps.push_back(make(context, guid, i + 1));
                s.first(std::move(steps));
                return Script::Step::Next;
            };
        }
    };
    out.push_back(Views::make(context, guid, 0));
    return out;
}

/// Pointing at an entity where the client's own picker finds it, and seeing
/// that it is what the pointer is on.
Steps pointAtEntity(ToolContext& context, uint64_t guid) {
    Steps out = bringIntoView(context, guid);
    auto tries = std::make_shared<int>(0);
    auto instead = std::make_shared<uint64_t>(0);
    struct Loop {
        static Script::Fn make(ToolContext& context, uint64_t guid, std::shared_ptr<int> tries,
                               std::shared_ptr<uint64_t> instead) {
            return [&context, guid, tries, instead](Script& s) {
                const auto at = clickPointOf(context, guid, instead.get());
                if (!at) {
                    if (++*tries > 40) {
                        auto* gh = context.app.getGameHandler();
                        const auto e = gh ? gh->getEntityManager().getEntity(guid) : nullptr;
                        std::string why = "no point of the window picks it";
                        if (*instead != 0) why += "; the client's picker finds " + guidText(*instead) + " there";
                        if (e) why += "; bearing " + json(rounded(bearingTo(context, entityAt(*e)), 0)).dump();
                        return s.fail(std::move(why));
                    }
                    return Script::Step::Again;
                }
                context.input.moveTo(*at);
                // It may have moved from under the pointer by the time the
                // frame has seen it.
                Steps check;
                check.push_back(Script::frames(2));
                check.push_back([&context, guid, tries, instead](Script& script) {
                    const glm::vec2 p = context.input.pointer();
                    if (!interfaceAt(context, p) && pickedAt(context, p) == guid) return Script::Step::Next;
                    if (++*tries > 40) return script.fail("it keeps moving from under the pointer");
                    Steps again;
                    again.push_back(make(context, guid, tries, instead));
                    script.first(std::move(again));
                    return Script::Step::Next;
                });
                s.first(std::move(check));
                return Script::Step::Next;
            };
        }
    };
    out.push_back(Loop::make(context, guid, tries, instead));
    return out;
}

/// The entity a call means, its guid; 0 with the reason where there is none.
uint64_t guidMeant(ToolContext& context, const json& args, std::string& bad) {
    if (!args.contains("guid") && !args.contains("name") && !args.contains("unit")) {
        bad = "a guid, a name or a unit is wanted";
        return 0;
    }
    const auto e = findEntity(context, args);
    if (!e) {
        bad = "no such entity in sight of the client";
        return 0;
    }
    return e->getGuid();
}

json targetNow(ToolContext& context) {
    auto* gh = context.app.getGameHandler();
    const auto e = gh ? gh->getEntityManager().getEntity(gh->getTargetGuid()) : nullptr;
    return e ? entityJson(context, *e, false) : json();
}

void target(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string which = args.value("which", std::string());
    if (!which.empty()) {
        // By the key its binding has, as a player cycles.
        const char* command = which == "nearest_enemy"    ? "TARGETNEARESTENEMY"
                              : which == "nearest_friend" ? "TARGETNEARESTFRIEND"
                              : which == "self"           ? "TARGETSELF"
                              : which == "last"           ? "TARGETLASTTARGET"
                              : which == "pet"            ? "TARGETPET"
                                                          : "";
        script.then(wantKeysFree(context));
        if (which == "none") {
            for (auto& fn : chatLine(context, "/cleartarget")) script.then(std::move(fn));
        } else {
            const json key = tryQueryLua(context, std::string("return GetBindingKey('") + command + "')");
            SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
            SDL_Keymod mods = SDL_KMOD_NONE;
            if (!key.is_string() || !parseChord(key.get<std::string>(), code, mods)) {
                script.then([command](Script& s) { return s.fail(std::string("no key is bound to ") + command); });
                return;
            }
            for (auto& fn : pressKey(context, code, mods, 0.05)) script.then(std::move(fn));
        }
    } else {
        std::string bad;
        const uint64_t guid = guidMeant(context, args, bad);
        if (guid == 0) {
            script.then([bad](Script& s) { return s.fail(bad); });
            return;
        }
        script.then(wantKeysFree(context));
        for (auto& fn : pointAtEntity(context, guid)) script.then(std::move(fn));
        for (auto& fn : pressButton(context, SDL_BUTTON_LEFT, SDL_KMOD_NONE, 1)) script.then(std::move(fn));
    }
    script.then(Script::seconds(0.2));
    script.then(finishWith(context, after, [&context] { return json{{"target", targetNow(context)}}; }));
}

void interact(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    std::string bad;
    const uint64_t guid = guidMeant(context, args, bad);
    if (guid == 0) {
        script.then([bad](Script& s) { return s.fail(bad); });
        return;
    }
    const auto here = [&context, guid]() -> std::optional<glm::vec3> {
        auto* gh = context.app.getGameHandler();
        const auto e = gh ? gh->getEntityManager().getEntity(guid) : nullptr;
        if (!e) return std::nullopt;
        return entityAt(*e);
    };
    auto walked = std::make_shared<json>();
    const auto within = static_cast<float>(args.value("within", 4.0));
    script.then(wantKeysFree(context));
    if (args.value("approach", true)) {
        script.then([&context, here, walked, within](Script& s) {
            const auto at = here();
            if (!at) return s.fail("it is gone");
            if (glm::length(*at - playerAt(context)) <= within) return Script::Step::Next;
            WalkOptions options;
            options.stopWithin = std::max(1.0f, within - 1.0f);
            options.timeout = 45.0;
            *walked = json::object();
            s.first(walkTo(context, here, options, walked));
            return Script::Step::Next;
        });
    }
    for (auto& fn : pointAtEntity(context, guid)) script.then(std::move(fn));
    const int button = buttonNamed(args.value("button", std::string("right")));
    for (auto& fn : pressButton(context, button, modsOf(args), 1)) script.then(std::move(fn));
    // Until something comes of it.
    const double patience = args.value("wait", 2.0);
    auto clickedAt = std::make_shared<double>(-1.0);
    script.then([&context, after, patience, clickedAt](Script& s) {
        if (*clickedAt < 0.0) *clickedAt = s.now();
        const bool heard = after.heard(
            context, {"GOSSIP_SHOW", "QUEST_GREETING", "QUEST_DETAIL", "QUEST_PROGRESS", "QUEST_COMPLETE",
                      "MERCHANT_SHOW", "TRAINER_SHOW", "LOOT_OPENED", "TAXIMAP_OPENED", "BANKFRAME_OPENED",
                      "AUCTION_HOUSE_SHOW", "MAIL_SHOW", "UI_ERROR_MESSAGE", "PLAYER_ENTER_COMBAT", "ITEM_TEXT_BEGIN",
                      "CONFIRM_BINDER", "UNIT_SPELLCAST_START", "TRADE_SKILL_SHOW", "PET_STABLE_SHOW"});
        return heard || s.now() - *clickedAt > patience ? Script::Step::Next : Script::Step::Again;
    });
    script.then(Script::seconds(0.25));
    script.then(finishWith(context, after, [&context, guid, walked] {
        json out = {{"clicked", guidText(guid)}};
        auto* gh = context.app.getGameHandler();
        if (const auto e = gh ? gh->getEntityManager().getEntity(guid) : nullptr) {
            out["name"] = entityName(context, *e);
            out["distance"] = rounded(glm::length(entityAt(*e) - playerAt(context)), 1);
        }
        if (!walked->is_null()) out["walk"] = *walked;
        json windows = json::array();
        for (const auto* open : openWindows(context)) windows.push_back(widgetLabel(*open));
        out["windows"] = std::move(windows);
        return out;
    }));
}

}  // namespace

void addPointTools(Toolbox& box) {
    using namespace schema;
    const json button = choice("left by default", {"left", "right", "middle"});
    const json modifiers = list("held down", choice("", {"shift", "ctrl", "alt"}), 3);
    box.push_back(handsTool("click_ui",
                            "Click a frame of the interface by name (or #id): the pointer is moved onto it and the "
                            "button pressed, as a player does.",
                            object({required("name", text("the frame", 96)), optional("button", button),
                                    optional("clicks", integer("2 for a double click", 1, 2)),
                                    optional("modifiers", modifiers),
                                    optional("settle", number("seconds to wait for what comes of it", 0, 10))}),
                            clickUi));
    box.push_back(handsTool("hover",
                            "Put the pointer on a frame, an entity or a point and read the tooltip.",
                            object({optional("name", text("a frame", 96)), optional("guid", guid("an entity")),
                                    optional("x", number("window pixels", 0, 16384)),
                                    optional("y", number("window pixels", 0, 16384)),
                                    optional("seconds", number("to wait, 0.3 by default", 0, 10))}),
                            hover));
    box.push_back(handsTool(
        "mouse", "The mouse by window pixels: move, click, down, up, drag (to to_x, to_y), wheel, or release it.",
        object({required("action", choice("", {"move", "click", "down", "up", "drag", "wheel", "release"})),
                optional("x", number("pixels from the left", 0, 16384)),
                optional("y", number("pixels from the top", 0, 16384)),
                optional("to_x", number("of a drag", 0, 16384)), optional("to_y", number("of a drag", 0, 16384)),
                optional("button", button), optional("clicks", integer("2 for a double click", 1, 2)),
                optional("modifiers", modifiers),
                optional("wheel", number("turns; up is positive", -20, 20))}),
        mouse, 30.0, Tool::Where::Anywhere));
    box.push_back(handsTool(
        "target",
        "Target something: an entity by a click on it in the world, or nearest_enemy / nearest_friend / self / last / "
        "pet by the key bound to it, or none.",
        object({optional("guid", guid("0x...")), optional("name", text("the nearest with this in its name", 64)),
                optional("unit", text("pet, mouseover, ...", 16)),
                optional("which", choice("", {"nearest_enemy", "nearest_friend", "self", "last", "pet", "none"}))}),
        target));
    Tool i = handsTool(
        "interact",
        "Right-click an entity in the world: talk to, loot, use, attack. Walks up to it first unless approach is false.",
        object({optional("guid", guid("0x...")), optional("name", text("the nearest with this in its name", 64)),
                optional("unit", text("target, npc, ...", 16)), optional("approach", flag("true by default")),
                optional("within", number("yards to walk up to, 4 by default", 1, 40)), optional("button", button),
                optional("modifiers", modifiers),
                optional("wait", number("seconds to wait for what comes of it, 2 by default", 0, 20))}),
        interact);
    i.patienceOf = [](const json& a) { return 75.0 + a.value("wait", 2.0); };
    box.push_back(std::move(i));
}

}  // namespace wowee::mcp
