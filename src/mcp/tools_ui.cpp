// The interface on the screen: windows, controls, dialogs, tooltips, errors.

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "core/application.hpp"
#include "mcp/game_view.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "mcp/ui_view.hpp"
#include "ui/widget_tree.hpp"

namespace wowee::mcp {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The lines a message frame shows now, oldest first.
json messageLines(ToolContext& context, const char* name, size_t limit) {
    json out = json::array();
    auto* tree = widgetTree(context);
    const auto* w = tree ? tree->findByName(name) : nullptr;
    if (!w) return out;
    std::vector<const ui::Widget::Message*> lines;
    for (const auto& m : w->messages) lines.push_back(&m);
    if (w->messagesInsertTop) std::reverse(lines.begin(), lines.end());
    const size_t from = lines.size() > limit ? lines.size() - limit : 0;
    for (size_t i = from; i < lines.size(); ++i) {
        out.push_back({{"text", plainText(lines[i]->text)}, {"age_s", rounded(lines[i]->age, 1)}});
    }
    return out;
}

json overview(ToolContext& context) {
    json out;
    json windows = json::array();
    for (const auto* w : openWindows(context)) {
        json j = widgetJson(context, *w);
        j["controls"] = controlsUnder(context, w->id, 200).size();
        windows.push_back(std::move(j));
    }
    out["windows"] = std::move(windows);
    out["popups"] = popups(context);
    if (json lines = tooltipLines(context, "GameTooltip"); !lines.is_null()) out["tooltip"] = std::move(lines);
    if (json lines = tooltipLines(context, "ItemRefTooltip"); !lines.is_null()) out["item_link"] = std::move(lines);
    out["errors"] = messageLines(context, "UIErrorsFrame", 5);
    if (context.input.pointerHeld()) {
        const glm::vec2 p = context.input.pointer();
        out["pointer"] = json::array({rounded(p.x, 0), rounded(p.y, 0)});
        if (const auto* under = widgetAt(context, p)) out["under_pointer"] = widgetLabel(*under);
    }
    const json more = tryQueryLua(context, R"lua(
local out = {}
local kind, id, link = GetCursorInfo()
if kind then out.cursor = {kind = kind, id = id, link = type(link) == "string" and link or nil} end
local box = GetCurrentKeyBoardFocus()
if box then out.edit_focus = box:GetName() or "unnamed" end
if SpellIsTargeting() then out.spell_waits_for_target = true end
return out
)lua");
    if (more.is_object()) out.update(more);
    return out;
}

/// A widget as an entry of a list; null for what says nothing by itself.
json entryOf(ToolContext& context, const ui::WidgetTree& tree, const ui::Widget& w) {
    if (!w.visible) return nullptr;
    if (w.kind == ui::WidgetKind::FontString) {
        // A control's own words are with the control.
        const auto* parent = tree.get(w.parent);
        if (w.text.empty() || w.tooltipLineOwner != 0 || (parent && isControl(*parent))) return nullptr;
        const std::string text = plainText(w.text);
        if (text.find_first_not_of(" \n\t") == std::string::npos) return nullptr;
        json entry = {{"text", text.substr(0, 600)}};
        if (!w.name.empty()) entry["name"] = w.name;
        return entry;
    }
    if (w.kind != ui::WidgetKind::Frame) return nullptr;
    if (isControl(w) || w.isStatusBar) return widgetJson(context, w);
    if (w.isMessageFrame && !w.messages.empty()) {
        return {{"name", widgetLabel(w)}, {"lines", messageLines(context, w.name.c_str(), 10)}};
    }
    if (w.isSimpleHtml && !w.text.empty()) {
        return {{"name", widgetLabel(w)}, {"text", plainText(w.text).substr(0, 2000)}};
    }
    return nullptr;
}

/// A list held to a filter and a limit.
struct EntryList {
    size_t limit;
    std::string filter;
    json list = json::array();
    size_t left = 0;

    void add(json entry) {
        if (entry.is_null()) return;
        if (!filter.empty() && lower(entry.dump(-1, ' ', false, json::error_handler_t::replace)).find(filter) ==
                                   std::string::npos) {
            return;
        }
        if (list.size() < limit) list.push_back(std::move(entry));
        else ++left;
    }
    json told(const char* name, json value) {
        json out = {{name, std::move(value)}, {"contents", std::move(list)}};
        if (left > 0) out["left_out"] = left;
        return out;
    }
};

/// Every text and control under a window, in the order it is laid out.
json windowContents(ToolContext& context, const ui::Widget& root, size_t limit, const std::string& filter) {
    auto* tree = widgetTree(context);
    EntryList list{.limit = limit, .filter = filter};
    std::vector<uint32_t> todo{root.id};
    while (!todo.empty()) {
        const uint32_t id = todo.back();
        todo.pop_back();
        const auto* w = tree->get(id);
        if (!w) continue;
        if (w->kind == ui::WidgetKind::Frame) {
            if (!w->visibleChain) continue;
            for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) todo.push_back(*it);
        }
        list.add(entryOf(context, *tree, *w));
    }
    return list.told("window", widgetLabel(root));
}

/// The same of everything that can be seen.
json filtered(ToolContext& context, size_t limit, const std::string& filter) {
    auto* tree = widgetTree(context);
    EntryList list{.limit = limit, .filter = filter};
    for (uint32_t id = tree->nextMaybeVisible(1); id != 0; id = tree->nextMaybeVisible(id + 1)) {
        if (const auto* w = tree->get(id)) list.add(entryOf(context, *tree, *w));
    }
    return list.told("filter", filter);
}

json uiTool(ToolContext& context, const json& args) {
    auto* tree = widgetTree(context);
    if (!tree) throw ToolError("the interface is not loaded");
    const auto limit = args.value("limit", static_cast<size_t>(80));
    const std::string filter = lower(args.value("filter", std::string()));
    if (args.contains("window")) {
        const auto* root = widgetNamed(context, args["window"].get<std::string>());
        if (!root) throw ToolError("no frame of that name");
        if (!root->visibleChain) throw ToolError("that frame is not shown");
        return windowContents(context, *root, limit, filter);
    }
    return filter.empty() ? overview(context) : filtered(context, limit, filter);
}

json uiAt(ToolContext& context, const json& args) {
    const glm::vec2 p(args["x"].get<float>(), args["y"].get<float>());
    json out;
    auto* tree = widgetTree(context);
    json chain = json::array();
    for (const auto* w = widgetAt(context, p); w && tree && chain.size() < 12; w = tree->get(w->parent)) {
        chain.push_back(widgetLabel(*w));
        if (w->parent == 0) break;
    }
    out["interface"] = chain;
    if (chain.empty()) {
        const uint64_t picked = pickedAt(context, p);
        out["world"] = picked != 0 ? json(guidText(picked)) : json();
    }
    return out;
}

}  // namespace

void addUiTools(Toolbox& box) {
    using namespace schema;
    box.push_back(lookTool(
        "ui",
        "The interface as it is on the screen. Without arguments: open windows, dialogs, tooltip, error texts, what "
        "the cursor holds. With `window` (a frame's name): its texts and controls with rects in window pixels, text, "
        "enabled, checked. With `filter`: every shown control or text with that in it.",
        object({optional("window", text("a frame's name, or #id", 96)),
                optional("filter", text("part of a name or text", 64)),
                optional("limit", integer("entries, 80 by default", 1, 400))}),
        uiTool));
    box.push_back(lookTool("ui_at", "What a click at a point of the window would land on: frames, or the world's entity.",
                           object({required("x", number("pixels from the left", 0, 16384)),
                                   required("y", number("pixels from the top", 0, 16384))}),
                           uiAt));
}

}  // namespace wowee::mcp
