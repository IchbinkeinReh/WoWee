// The keyboard: keys, bindings, text, slash commands, action buttons, spells
// and items.

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

#include "core/application.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "mcp/game_view.hpp"
#include "mcp/hands.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/send_guard.hpp"
#include "mcp/tool_context.hpp"
#include "mcp/ui_view.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/widget_tree.hpp"

namespace wowee::mcp {

namespace {

json windowsNow(ToolContext& context) {
    json windows = json::array();
    for (const auto* open : openWindows(context)) windows.push_back(widgetLabel(*open));
    return json{{"windows", std::move(windows)}};
}

void failNow(Script& script, std::string why) {
    script.then([why = std::move(why)](Script& s) { return s.fail(why); });
}

GuardContext guardOf(ToolContext& context) {
    GuardContext g;
    auto* gh = context.app.getGameHandler();
    if (!gh) return g;
    g.ownGuid = gh->getPlayerGuid();
    if (const auto* c = gh->getActiveCharacter()) g.ownName = c->name;
    g.targetGuid = gh->getTargetGuid();
    g.selectedOnRealm = context.selectedOnRealm;
    g.allow = context.options.allow;
    return g;
}

void key(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
    SDL_Keymod mods = SDL_KMOD_NONE;
    const std::string chord = args["key"].get<std::string>();
    if (screenIsTheUsers(context)) return failNow(script, "no keys on the screen " + clientState(context));
    if (!parseChord(chord, code, mods)) return failNow(script, "no such key: " + chord);
    if (code == SDL_SCANCODE_F4 && (mods & SDL_KMOD_LALT)) return failNow(script, "the quit tool ends the client");
    const int times = args.value("times", 1);
    for (int i = 0; i < times; ++i) {
        for (auto& fn : pressKey(context, code, mods, args.value("seconds", 0.05))) script.then(std::move(fn));
    }
    script.then(Script::seconds(args.value("settle", 0.25)));
    script.then(finishWith(context, after, [&context] { return windowsNow(context); }));
}

void binding(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string command = args["command"].get<std::string>();
    const json bound = tryQueryLua(context, "return GetBindingKey(" + luaQuote(command) + ")");
    if (!bound.is_string()) return failNow(script, "no key is bound to " + command);
    SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
    SDL_Keymod mods = SDL_KMOD_NONE;
    if (!parseChord(bound.get<std::string>(), code, mods)) {
        return failNow(script, "the key " + bound.get<std::string>() + " is none this server can press");
    }
    script.then(wantKeysFree(context));
    for (auto& fn : pressKey(context, code, mods, args.value("seconds", 0.05))) script.then(std::move(fn));
    script.then(Script::seconds(args.value("settle", 0.4)));
    script.then(finishWith(context, after, [&context, bound] {
        json out = windowsNow(context);
        out["key"] = bound;
        return out;
    }));
}

void typeText(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string text = args["text"].get<std::string>();
    if (const Verdict v = judgeChatText(text, context.options.lua); !v.allowed()) {
        context.refusals.push({.what = "type_text", .why = v.why, .at = context.now});
        return failNow(script, v.why);
    }
    if (args.contains("box")) {
        const std::string name = args["box"].get<std::string>();
        Script* self = &script;
        for (auto& fn : clickAt(
                 context,
                 [&context, name, self]() -> std::optional<glm::vec2> {
                     const auto* w = widgetNamed(context, name);
                     if (!w || !w->visible) {
                         self->failed = true;
                         self->result = name + " is not shown";
                         return std::nullopt;
                     }
                     return clickPointOf(context, *w);
                 },
                 SDL_BUTTON_LEFT, SDL_KMOD_NONE, 1)) {
            script.then(std::move(fn));
        }
    }
    script.then([&context](Script& s) {
        (void)context;
        return ui::interfaceTakingTypedInput() ? Script::Step::Next : s.fail("no edit box has the keyboard");
    });
    if (args.value("replace", false)) {
        for (auto& fn : pressKey(context, scancodeNamed("A"), SDL_KMOD_LCTRL, 0.05)) script.then(std::move(fn));
        for (auto& fn : pressKey(context, SDL_SCANCODE_BACKSPACE, SDL_KMOD_NONE, 0.05)) script.then(std::move(fn));
    }
    script.then([&context, text](Script&) {
        context.input.type(text);
        return Script::Step::Next;
    });
    script.then(Script::frames(3));
    if (args.value("submit", false)) {
        for (auto& fn : pressKey(context, SDL_SCANCODE_RETURN, SDL_KMOD_NONE, 0.05)) script.then(std::move(fn));
    }
    script.then(Script::seconds(0.2));
    script.then(finishWith(context, after, [&context] {
        json out = windowsNow(context);
        const json box = tryQueryLua(context, "local b = GetCurrentKeyBoardFocus() "
                                              "if b then return {box = b:GetName(), text = b:GetText()} end");
        if (box.is_object()) out.update(box);
        return out;
    }));
}

void slash(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string text = args["command"].get<std::string>();
    if (text.empty() || text[0] != '/') return failNow(script, "a slash command begins with /");
    if (const Verdict v = judgeChatText(text, context.options.lua); !v.allowed()) {
        context.refusals.push({.what = text.substr(0, 40), .why = v.why, .at = context.now});
        return failNow(script, v.why);
    }
    for (auto& fn : chatLine(context, text)) script.then(std::move(fn));
    script.then(Script::seconds(args.value("settle", 0.5)));
    script.then(finishWith(context, after, [&context] { return windowsNow(context); }));
}

/// Whether a command answers in chat lines, which the realm may take a second
/// or two over.
bool gmCommandAnswers(const std::string& command) {
    for (const char* word : {".lookup", ".list", " info", " near", ".gps", ".server", ".guid", ".distance"}) {
        if (command.find(word) != std::string::npos) return true;
    }
    return false;
}

void gm(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string command = args["command"].get<std::string>();
    GuardContext guard = guardOf(context);
    if (command.empty() || command[0] != '.') return failNow(script, "a game master's command begins with .");
    // Whom it is about is who is selected: the character, but for a command
    // that only tells.
    const bool onSelf = args.value("on", std::string(gmCommandOnlyTells(command) ? "target" : "self")) == "self";
    if (onSelf) guard.targetGuid = guard.selectedOnRealm = guard.ownGuid;
    if (const Verdict v = judgeGmCommand(command, guard); !v.allowed()) {
        context.refusals.push({.what = command.substr(0, 40), .why = v.why, .at = context.now});
        return failNow(script, v.why);
    }
    if (guard.ownName.empty()) return failNow(script, "the character's name is not known");
    auto selected = std::make_shared<std::optional<uint64_t>>();
    script.then([&context, onSelf, selected](Script&) {
        auto* gh = context.app.getGameHandler();
        if (!onSelf || !gh || gh->getTargetGuid() == gh->getPlayerGuid()) return Script::Step::Next;
        // Also with nothing selected: the realm takes no selection as the
        // character only for some commands - .die, .damage and the like say
        // 'select a character or creature' - so the character is selected for
        // the command and the selection cleared again after it.
        *selected = gh->getTargetGuid();
        // The realm is told before the command: the selection's packet goes
        // out here, the whisper's after it.
        gh->targetUnit(gh->getPlayerGuid());
        return Script::Step::Next;
    });
    // A whisper to oneself, which the realm takes a command out of. It is no
    // act of a player's, so it is not typed: a dialog or an edit box that has
    // the keyboard is not in its way.
    script.then([&context, guard, command](Script& s) {
        context.origin.actedByServer();
        try {
            queryLua(context, "SendChatMessage(" + luaQuote(command) + ", 'WHISPER', nil, " + luaQuote(guard.ownName) + ")");
        } catch (const ToolError& e) {
            return s.fail(e.what());
        }
        return Script::Step::Next;
    });
    // At least `settle`, and for a command that answers in lines until they
    // have come and stopped coming.
    const double settle = args.value("settle", 1.0);
    const double patience = std::max(settle, gmCommandAnswers(command) ? 4.0 : 0.0);
    auto heard = std::make_shared<std::pair<uint64_t, double>>(after.chat, 0.0);
    script.then([&context, settle, patience, heard, began = -1.0](Script& s) mutable {
        if (began < 0.0) began = s.now();
        if (const uint64_t last = context.chat.last(); last != heard->first) *heard = {last, s.now()};
        const double passed = s.now() - began;
        if (passed >= patience) return Script::Step::Next;
        const bool answered = heard->second > 0.0 && s.now() - heard->second >= 0.4;
        return passed >= settle && (answered || patience <= settle) ? Script::Step::Next : Script::Step::Again;
    });
    script.then([&context, selected](Script&) {
        auto* gh = context.app.getGameHandler();
        // Back to what was selected, where it is still there and nothing else
        // has been selected since.
        if (!gh || !*selected || gh->getTargetGuid() != gh->getPlayerGuid()) return Script::Step::Next;
        if (**selected == 0) gh->clearTarget();
        else if (gh->getEntityManager().getEntity(**selected)) gh->targetUnit(**selected);
        return Script::Step::Next;
    });
    script.then(finishWith(context, after, [&context, onSelf] {
        return json{{"pos", vec3Json(playerAt(context))},
                    {"facing", rounded(playerFacing(context), 4)},
                    {"on", onSelf ? "self" : "target"}};
    }));
}

/// The button of the action bars that has a spell or item of this name, or a
/// slot of this number, or is called this: its name and its key.
json actionButton(ToolContext& context, const json& args) {
    std::string wanted = "''";
    std::string slot = "nil";
    std::string button = "nil";
    if (args.contains("spell")) wanted = luaQuote(args["spell"].get<std::string>());
    if (args.contains("slot")) slot = std::to_string(args["slot"].get<int>());
    if (args.contains("button")) button = luaQuote(args["button"].get<std::string>());
    return tryQueryLua(context, "local wanted, slot, named = string.lower(" + wanted + "), " + slot + ", " + button + R"lua(
local bars = {{"ActionButton", "ACTIONBUTTON"}, {"BonusActionButton", "ACTIONBUTTON"},
  {"MultiBarBottomLeftButton", "MULTIACTIONBAR1BUTTON"}, {"MultiBarBottomRightButton", "MULTIACTIONBAR2BUTTON"},
  {"MultiBarRightButton", "MULTIACTIONBAR3BUTTON"}, {"MultiBarLeftButton", "MULTIACTIONBAR4BUTTON"}}
local found
for _, bar in ipairs(bars) do
  for i = 1, 12 do
    local b = getglobal(bar[1] .. i)
    local action = b and b.action
    if action and HasAction(action) then
      local kind, id, _, spell = GetActionInfo(action)
      local name = spell and GetSpellInfo(spell) or kind == "item" and GetItemInfo(id) or GetActionText(action)
      local match = (named and bar[1] .. i == named) or (slot and action == slot)
        or (wanted ~= "" and name and string.lower(name) == wanted)
      if match and (b:IsVisible() or not found) then
        local usable, nomana = IsUsableAction(action)
        found = {button = bar[1] .. i, key = GetBindingKey(bar[2] .. i), name = name, slot = action,
          shown = b:IsVisible() and true or false, usable = usable and true or false, no_mana = nomana and true or nil,
          in_range = IsActionInRange(action)}
        if b:IsVisible() then return found end
      end
    end
  end
end
return found
)lua");
}

void useAction(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    if (!args.contains("slot") && !args.contains("button") && !args.contains("spell")) {
        return failNow(script, "a slot, a button or a spell is wanted");
    }
    const json found = actionButton(context, args);
    const bool byKey = args.value("method", std::string("key")) == "key";
    if (!found.is_object()) {
        if (!args.contains("spell")) return failNow(script, "nothing is on that button");
        // Not on a bar: by name, as it is typed.
        for (auto& fn : chatLine(context, "/cast " + args["spell"].get<std::string>())) script.then(std::move(fn));
    } else {
        SDL_Scancode code = SDL_SCANCODE_UNKNOWN;
        SDL_Keymod mods = SDL_KMOD_NONE;
        const bool hasKey =
            found.contains("key") && found["key"].is_string() && parseChord(found["key"].get<std::string>(), code, mods);
        if (byKey && hasKey) {
            script.then(wantKeysFree(context));
            for (auto& fn : pressKey(context, code, mods, 0.05)) script.then(std::move(fn));
        } else {
            const std::string name = found["button"].get<std::string>();
            for (auto& fn : clickAt(
                     context,
                     [&context, name]() -> std::optional<glm::vec2> {
                         const auto* w = widgetNamed(context, name);
                         return w ? clickPointOf(context, *w) : std::nullopt;
                     },
                     SDL_BUTTON_LEFT, SDL_KMOD_NONE, 1)) {
                script.then(std::move(fn));
            }
        }
    }
    const double patience = args.value("wait", 1.0);
    auto since = std::make_shared<double>(-1.0);
    script.then([&context, after, patience, since](Script& s) {
        if (*since < 0.0) *since = s.now();
        const bool ended = after.heard(context, {"UNIT_SPELLCAST_SUCCEEDED", "UNIT_SPELLCAST_FAILED",
                                                 "UNIT_SPELLCAST_INTERRUPTED", "UI_ERROR_MESSAGE"});
        return ended || s.now() - *since > patience ? Script::Step::Next : Script::Step::Again;
    });
    script.then(Script::frames(3));
    script.then(finishWith(context, after, [found] {
        return json{{"action", found.is_object() ? found : json("/cast")}};
    }));
}

void useItem(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string wanted = args.contains("name") ? luaQuote(args["name"].get<std::string>()) : "nil";
    const std::string bag = args.contains("bag") ? std::to_string(args["bag"].get<int>()) : "nil";
    const std::string slot = args.contains("slot") ? std::to_string(args["slot"].get<int>()) : "nil";
    if (wanted == "nil" && (bag == "nil" || slot == "nil")) return failNow(script, "a name, or a bag and a slot");
    const std::string where = "local name, bag, slot = " + wanted + ", " + bag + ", " + slot + R"lua(
if name then
  for b = 0, 4 do
    for s = 1, GetContainerNumSlots(b) do
      local link = GetContainerItemLink(b, s)
      if link and string.find(string.lower(link), "%[" .. string.gsub(string.lower(name), "(%W)", "%%%1") .. "%]") then
        bag, slot = b, s
        break
      end
    end
    if bag then break end
  end
end
if not bag then return nil end
local out = {bag = bag, slot = slot}
for i = 1, 13 do
  local frame = getglobal("ContainerFrame" .. i)
  if frame and frame:IsShown() and frame:GetID() == bag then
    for n = 1, 36 do
      local item = getglobal("ContainerFrame" .. i .. "Item" .. n)
      if item and item:IsShown() and item:GetID() == slot then out.button = item:GetName() end
    end
  end
end
return out
)lua";
    const json found = tryQueryLua(context, where);
    if (!found.is_object()) return failNow(script, "no such item in the bags");
    if (!found.contains("button")) {
        // Its bag opened with the button a player opens it with.
        const int b = found["bag"].get<int>();
        const std::string opener = b == 0 ? "MainMenuBarBackpackButton" : "CharacterBag" + std::to_string(b - 1) + "Slot";
        for (auto& fn : clickAt(
                 context,
                 [&context, opener]() -> std::optional<glm::vec2> {
                     const auto* w = widgetNamed(context, opener);
                     return w ? clickPointOf(context, *w) : std::nullopt;
                 },
                 SDL_BUTTON_LEFT, SDL_KMOD_NONE, 1)) {
            script.then(std::move(fn));
        }
        script.then(Script::seconds(0.4));
    }
    const std::string how = args.value("how", std::string("use"));
    Script* self = &script;
    auto pressed = std::make_shared<std::string>();
    for (auto& fn : movePointer(context, [&context, where, self, pressed]() -> std::optional<glm::vec2> {
             const json now = tryQueryLua(context, where);
             if (!now.is_object() || !now.contains("button")) {
                 self->failed = true;
                 self->result = "its bag did not open";
                 return std::nullopt;
             }
             *pressed = now["button"].get<std::string>();
             const auto* w = widgetNamed(context, *pressed);
             const auto at = w ? clickPointOf(context, *w) : std::nullopt;
             if (!at) {
                 self->failed = true;
                 self->result = *pressed + " cannot be clicked";
             }
             return at;
         })) {
        script.then(std::move(fn));
    }
    for (auto& fn : pressButton(context, how == "use" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT, SDL_KMOD_NONE, 1)) {
        script.then(std::move(fn));
    }
    script.then(Script::seconds(args.value("wait", 0.6)));
    script.then(finishWith(context, after, [pressed] { return json{{"clicked", *pressed}}; }));
}

}  // namespace

void addKeyTools(Toolbox& box) {
    using namespace schema;
    Tool k = handsTool("key",
                       "Press a key, by the key bindings' names: W, SPACE, ESCAPE, ENTER, TAB, F1, NUMPAD1, "
                       "CTRL-SHIFT-A ...",
                       object({required("key", text("the key, with ALT-, CTRL-, SHIFT- before it", 32)),
                               optional("seconds", number("held, 0.05 by default", 0.02, 20)),
                               optional("times", integer("how often", 1, 20)),
                               optional("settle", number("seconds to wait for what comes of it", 0, 10))}),
                       key, 30.0, Tool::Where::Anywhere);
    k.patienceOf = [](const json& a) { return (a.value("seconds", 0.05) + 1.0) * a.value("times", 1) + 15.0; };
    box.push_back(std::move(k));
    box.push_back(handsTool("binding",
                            "Press the key bound to a command: TOGGLEBACKPACK, OPENALLBAGS, TOGGLESPELLBOOK, "
                            "TOGGLEQUESTLOG, TOGGLECHARACTER0, TOGGLETALENTS, TOGGLEWORLDMAP, TOGGLEGAMEMENU, "
                            "ACTIONBUTTON1, TOGGLESHEATH, ...",
                            object({required("command", text("the command", 64)),
                                    optional("seconds", number("held", 0.02, 20)),
                                    optional("settle", number("seconds to wait for what comes of it", 0, 10))}),
                            binding));
    box.push_back(handsTool(
        "type_text", "Type into the edit box that has the keyboard, or into `box` after clicking it.",
        object({required("text", text("what to type", 500)), optional("box", text("an edit box's name", 96)),
                optional("replace", flag("what is in it is deleted first")),
                optional("submit", flag("press Enter after it"))}),
        typeText));
    box.push_back(handsTool("slash",
                            "Type a slash command into the chat box and press Enter. Talk to other players and /run "
                            "are refused unless the server was started for them.",
                            object({required("command", text("/sit, /target Name, /cast Spell, ...", 255)),
                                    optional("settle", number("seconds to wait for what comes of it", 0, 10))}),
                            slash));
    box.push_back(handsTool("gm",
                            "A game master's command for the own character, whispered to itself: .revive, .gps, "
                            ".go xyz, .tele, .gm on/off, .modify ..., .additem id. Only a list of harmless ones "
                            "(docs/MCP_SERVER.md), and never on anybody else: but for those that only tell (.npc info, "
                            ".gps, .lookup ...) they are given with the character selected, and what was selected is "
                            "selected again after; on=target with anything else selected is refused. A dialog may be "
                            "open.",
                            object({required("command", text("beginning with a dot", 200)),
                                    optional("on", choice("whom it is about", {"self", "target"})),
                                    optional("settle", number("seconds to wait for the answer", 0, 15))}),
                            gm));
    Tool a = handsTool(
        "use_action",
        "Use an action button by its key (or a click): the one a spell or item is on, a slot's, or one by name. A "
        "spell on no bar is cast by /cast.",
        object({optional("spell", text("a spell's or item's name", 64)), optional("slot", integer("1 to 120", 1, 120)),
                optional("button", text("ActionButton3, ...", 64)),
                optional("method", choice("key by default", {"key", "click"})),
                optional("wait", number("seconds to wait for the cast to end, 1 by default", 0, 30))}),
        useAction);
    a.patienceOf = [](const json& j) { return 20.0 + j.value("wait", 1.0); };
    box.push_back(std::move(a));
    box.push_back(handsTool("use_item",
                            "Right-click an item in its bag (opened first if need be), or pick it up with a left "
                            "click.",
                            object({optional("name", text("its whole name", 64)), optional("bag", integer("0 to 4", 0, 4)),
                                    optional("slot", integer("from 1", 1, 36)),
                                    optional("how", choice("use by default", {"use", "pick_up"})),
                                    optional("wait", number("seconds to wait for what comes of it", 0, 10))}),
                            useItem));
}

}  // namespace wowee::mcp
