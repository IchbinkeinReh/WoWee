// What can be done now: each with the tool and the arguments that do it.

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/application.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "mcp/game_view.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "mcp/ui_view.hpp"
#include "ui/widget_tree.hpp"

namespace wowee::mcp {

namespace {

json action(const std::string& what, const char* tool, json arguments) {
    return {{"do", what}, {"tool", tool}, {"arguments", std::move(arguments)}};
}

/// The buttons of the open windows: what a click on each would be.
json uiActions(ToolContext& context) {
    json out = json::array();
    const auto add = [&](const ui::Widget& w, const std::string& window) {
        if (!w.enabled || out.size() >= 60) return;
        const std::string text = widgetText(context, w);
        if (w.isEditBox) {
            out.push_back(action("type into " + widgetLabel(w) + (text.empty() ? "" : " (\"" + text + "\")"),
                                 "type_text", {{"box", widgetLabel(w)}, {"text", "..."}}));
            return;
        }
        out.push_back(action("press " + (text.empty() ? widgetLabel(w) : "\"" + text + "\"") + " in " + window,
                             "click_ui", {{"name", widgetLabel(w)}}));
    };
    auto* tree = widgetTree(context);
    for (int i = 1; tree && i <= 4; ++i) {
        const auto* popup = tree->findByName("StaticPopup" + std::to_string(i));
        if (!popup || !popup->visible) continue;
        for (const auto* c : controlsUnder(context, popup->id, 8)) {
            if (c->enabled) add(*c, "the dialog \"" + widgetText(context, *popup) + "\"");
        }
    }
    for (const auto* w : openWindows(context)) {
        if (w->name.rfind("StaticPopup", 0) == 0) continue;
        for (const auto* c : controlsUnder(context, w->id, 40)) {
            // A slot of a bag with nothing in it, a scroll bar's end: no use
            // in a list of what there is to do.
            if (c->isSlider || c->objectType == "ColorSelect") continue;
            if (widgetText(context, *c).empty() && c->name.find("ContainerFrame") == 0) continue;
            add(*c, widgetLabel(*w));
        }
    }
    return out;
}

json entityActions(ToolContext& context, double radius) {
    json out = json::array();
    auto* gh = context.app.getGameHandler();
    if (!gh) return out;
    std::vector<std::pair<float, std::shared_ptr<game::Entity>>> sorted;
    const glm::vec3 me = playerAt(context);
    for (const auto& [guid, e] : gh->getEntityManager().getEntities()) {
        if (!e || guid == gh->getPlayerGuid()) continue;
        const float d = glm::length(entityAt(context, *e) - me);
        if (d <= radius) sorted.emplace_back(d, e);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [d, e] : sorted) {
        if (out.size() >= 25) break;
        const json j = entityJson(context, *e, false);
        const std::string name = j.value("name", std::string());
        const std::string guid = j["guid"].get<std::string>();
        const std::string away = " (" + j["distance"].dump() + " yd, bearing " + j["bearing"].dump() + ")";
        if (j.value("selectable", true) == false) continue;
        if (e->getType() == game::ObjectType::GAMEOBJECT) {
            if (j.value("usable", false)) {
                out.push_back(action("use " + name + away, "interact", {{"guid", guid}}));
            }
        } else if (e->getType() == game::ObjectType::UNIT) {
            if (j.value("lootable", false)) {
                out.push_back(action("loot " + name + away, "interact", {{"guid", guid}}));
            } else if (j.value("dead", false)) {
                continue;
            } else if (j.contains("roles") && !j["roles"].empty() && !j.value("attackable", false)) {
                std::string roles;
                for (const auto& r : j["roles"]) roles += (roles.empty() ? "" : ", ") + r.get<std::string>();
                out.push_back(action("talk to " + name + " [" + roles + "]" +
                                         (j.contains("quest") ? " quest " + j["quest"].get<std::string>() : "") + away,
                                     "interact", {{"guid", guid}}));
            } else if (j.value("attackable", false)) {
                out.push_back(action("attack " + name + " (level " + j["level"].dump() + ", " +
                                         j.value("reaction", std::string()) + ")" + away,
                                     "interact", {{"guid", guid}}));
            }
        }
    }
    return out;
}

json spellActions(ToolContext& context) {
    json out = json::array();
    const json usable = tryQueryLua(context, R"lua(
local out, seen = {}, {}
for slot = 1, 120 do
  if HasAction(slot) then
    local kind, id = GetActionInfo(slot)
    local usable = IsUsableAction(slot)
    local start, duration = GetActionCooldown(slot)
    local ready = not duration or duration <= 1.5 or start + duration <= GetTime()
    local name = kind == "spell" and GetSpellInfo(id) or kind == "item" and GetItemInfo(id) or GetActionText(slot)
    if usable and ready and name and not seen[name] then
      seen[name] = true
      out[#out + 1] = {name = name, kind = kind, in_range = IsActionInRange(slot)}
    end
  end
end
return out
)lua");
    if (!usable.is_array()) return out;
    for (const auto& a : usable) {
        if (!a.contains("name") || !a["name"].is_string()) continue;
        std::string what = (a.value("kind", std::string()) == "item" ? "use " : "cast ") + a["name"].get<std::string>();
        if (a.contains("in_range") && a["in_range"] == 0) what += " (target out of range)";
        out.push_back(action(what, "use_action", {{"spell", a["name"]}}));
        if (out.size() >= 30) break;
    }
    return out;
}

json actions(ToolContext& context, const json& args) {
    json out;
    const std::string state = clientState(context);
    out["client"] = state;
    if (state == "char_select") {
        out["client_actions"] = json::array({action("enter the world with the selected character", "enter_world",
                                                    json::object())});
        return out;
    }
    if (!inWorld(context)) {
        out["client_actions"] = json::array({action("wait for the world", "wait", {{"client", "in_world"}, {"timeout", 60}})});
        return out;
    }
    out["interface"] = uiActions(context);
    out["world"] = entityActions(context, args.value("radius", 40.0));
    out["abilities"] = spellActions(context);
    json more = json::array();
    if (state == "dead") more.push_back(action("release the spirit, or wait", "ui", json::object()));
    more.push_back(action("walk to a place or an entity", "walk_to", {{"to", vec3Json(playerAt(context))}}));
    more.push_back(action("hold movement keys", "move", {{"directions", json::array({"forward"})}, {"seconds", 1}}));
    more.push_back(action("turn", "turn", {{"degrees", 90}}));
    more.push_back(action("jump", "jump", json::object()));
    more.push_back(action("target the nearest enemy", "target", {{"which", "nearest_enemy"}}));
    for (const auto& [what, command] :
         {std::pair{"open all bags", "OPENALLBAGS"}, {"open or close the spellbook", "TOGGLESPELLBOOK"},
          {"open or close the quest log", "TOGGLEQUESTLOG"}, {"open or close the character", "TOGGLECHARACTER0"},
          {"open or close the talents", "TOGGLETALENTS"}, {"open or close the world map", "TOGGLEWORLDMAP"},
          {"open the game menu, or close windows", "TOGGLEGAMEMENU"}}) {
        more.push_back(action(what, "binding", {{"command", command}}));
    }
    out["always"] = std::move(more);
    return out;
}

}  // namespace

void addActionsTool(Toolbox& box) {
    using namespace schema;
    box.push_back(lookTool(
        "actions",
        "What can be done now, each with the tool and the arguments that do it: buttons of open windows, entities to "
        "talk to, loot, use or attack, abilities that are ready, and the movements and windows there always are.",
        object({optional("radius", number("yards for entities, 40 by default", 1, 200))}), actions,
        Tool::Where::Anywhere));
}

}  // namespace wowee::mcp
