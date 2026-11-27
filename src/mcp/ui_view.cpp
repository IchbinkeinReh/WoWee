#include "mcp/ui_view.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "mcp/game_view.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/tool_context.hpp"
#include "ui/widget_tree.hpp"

namespace wowee::mcp {

namespace {

constexpr size_t kLongestText = 300;

/// Part of the screen at all times; windows are what is opened over it.
constexpr std::array<std::string_view, 37> kFurniture = {
    "MainMenuBar", "MultiBar", "MainMenu", "AlwaysUpFrame", "TutorialFrameAlert", "PlayerFrame", "TargetFrame", "FocusFrame", "PetFrame", "PartyMemberFrame",
    "MinimapCluster", "BuffFrame", "ChatFrame", "DEFAULT_CHAT_FRAME", "FriendsMicroButton", "ChatFrameMenuButton",
    "UIErrorsFrame", "WatchFrame", "DurabilityFrame", "VehicleMenuBar", "MirrorTimer", "CastingBarFrame",
    "WorldStateAlwaysUpFrame", "GeneralDockManager", "ComboFrame", "TemporaryEnchantFrame", "ConsolidatedBuffs",
    "QuestTimerFrame", "ZoneTextFrame", "SubZoneTextFrame", "AutoFollowStatus", "RaidWarningFrame",
    "RaidBossEmoteFrame", "WorldFrame", "BonusActionBarFrame", "TotemFrame", "RuneFrame"};

bool isFurniture(const std::string& name) {
    return std::any_of(kFurniture.begin(), kFurniture.end(),
                       [&](std::string_view f) { return name.compare(0, f.size(), f) == 0; });
}

bool isRegion(const ui::Widget& w) {
    return w.kind != ui::WidgetKind::Frame;
}

std::string trimmed(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\t')) --b;
    std::string out = s.substr(a, b - a);
    if (out.size() > kLongestText) out = out.substr(0, kLongestText) + "...";
    return out;
}

}  // namespace

ui::WidgetTree* widgetTree(ToolContext& context) {
    auto* manager = context.app.getAddonManager();
    auto* engine = manager ? manager->getLuaEngine() : nullptr;
    if (!engine || !engine->isInitialized()) return nullptr;
    return &engine->widgets();
}

std::string widgetLabel(const ui::Widget& widget) {
    return widget.name.empty() ? "#" + std::to_string(widget.id) : widget.name;
}

const ui::Widget* widgetNamed(ToolContext& context, const std::string& name) {
    auto* tree = widgetTree(context);
    if (!tree || name.empty()) return nullptr;
    if (name[0] == '#') {
        const unsigned long id = std::strtoul(name.c_str() + 1, nullptr, 10);
        return id != 0 && id < tree->size() ? tree->get(static_cast<uint32_t>(id)) : nullptr;
    }
    return tree->findByName(name);
}

glm::vec4 widgetRect(ToolContext& context, const ui::Widget& widget) {
    auto* tree = widgetTree(context);
    auto* window = context.app.getWindow();
    if (!tree || !window) return glm::vec4(0.0f);
    const float scale = tree->uiScale() > 0.0f ? tree->uiScale() : 1.0f;
    const auto screenH = static_cast<float>(window->getHeight());
    // The tree measures up from the bottom; the window down from the top.
    return glm::vec4(widget.left * scale, screenH - (widget.bottom + widget.rectH) * scale, widget.rectW * scale,
                     widget.rectH * scale);
}

bool isControl(const ui::Widget& w) {
    if (isRegion(w) || !w.mouseEnabled) return false;
    return w.objectType == "Button" || w.objectType == "CheckButton" || w.isEditBox || w.isSlider ||
           w.objectType == "ColorSelect";
}

std::string widgetText(ToolContext& context, const ui::Widget& widget) {
    auto* tree = widgetTree(context);
    if (!tree) return {};
    if (widget.isEditBox) return trimmed(widget.editText);
    std::string out = trimmed(plainText(widget.text));
    for (const uint32_t id : widget.children) {
        const auto* child = tree->get(id);
        if (!child || child->kind != ui::WidgetKind::FontString || !child->visible || child->text.empty()) continue;
        const std::string text = trimmed(plainText(child->text));
        if (text.empty()) continue;
        if (!out.empty()) out += " | ";
        out += text;
        if (out.size() > kLongestText) break;
    }
    return out;
}

const ui::Widget* widgetAt(ToolContext& context, const glm::vec2& pixel) {
    auto* tree = widgetTree(context);
    auto* window = context.app.getWindow();
    if (!tree || !window) return nullptr;
    const float scale = tree->uiScale() > 0.0f ? tree->uiScale() : 1.0f;
    const uint32_t hit =
        tree->hitTest(pixel.x / scale, (static_cast<float>(window->getHeight()) - pixel.y) / scale);
    return hit != 0 ? tree->get(hit) : nullptr;
}

bool landsOn(ToolContext& context, const glm::vec2& pixel, const ui::Widget& widget) {
    auto* tree = widgetTree(context);
    const ui::Widget* hit = widgetAt(context, pixel);
    for (int depth = 0; tree && hit && depth < 64; ++depth) {
        if (hit->id == widget.id) return true;
        if (hit->parent == 0) break;
        hit = tree->get(hit->parent);
    }
    return false;
}

std::optional<glm::vec2> clickPointOf(ToolContext& context, const ui::Widget& widget, std::string* covering) {
    auto* window = context.app.getWindow();
    if (!window || !widget.visible) return std::nullopt;
    const glm::vec4 r = widgetRect(context, widget);
    // The middle, then toward the corners and the edges.
    constexpr std::array<glm::vec2, 9> kAt = {{{0.5f, 0.5f}, {0.3f, 0.5f}, {0.7f, 0.5f}, {0.5f, 0.3f}, {0.5f, 0.7f},
                                               {0.2f, 0.2f}, {0.8f, 0.2f}, {0.2f, 0.8f}, {0.8f, 0.8f}}};
    for (const auto& at : kAt) {
        const glm::vec2 p(r.x + r.z * at.x, r.y + r.w * at.y);
        if (p.x < 0.0f || p.y < 0.0f || p.x >= static_cast<float>(window->getWidth()) ||
            p.y >= static_cast<float>(window->getHeight())) {
            continue;
        }
        if (landsOn(context, p, widget)) return p;
        if (covering && covering->empty()) {
            const auto* other = widgetAt(context, p);
            *covering = other ? widgetLabel(*other) : "the world";
        }
    }
    return std::nullopt;
}

json widgetJson(ToolContext& context, const ui::Widget& widget) {
    const glm::vec4 r = widgetRect(context, widget);
    json out = {{"name", widgetLabel(widget)},
                {"type", widget.objectType},
                {"rect", json::array({rounded(r.x, 0), rounded(r.y, 0), rounded(r.z, 0), rounded(r.w, 0)})}};
    if (const std::string text = widgetText(context, widget); !text.empty()) out["text"] = text;
    if (!widget.enabled) out["enabled"] = false;
    if (widget.objectType == "CheckButton") out["checked"] = widget.checked;
    if (widget.isSlider || widget.isStatusBar) {
        out["value"] = rounded(widget.barValue, 3);
        out["range"] = json::array({rounded(widget.barMin, 3), rounded(widget.barMax, 3)});
    }
    const float ownAlpha = widget.kind == ui::WidgetKind::Frame ? widget.alpha : widget.color[3];
    if (ownAlpha < 0.99f) out["alpha"] = rounded(ownAlpha, 2);
    return out;
}

std::vector<const ui::Widget*> openWindows(ToolContext& context) {
    std::vector<const ui::Widget*> out;
    auto* tree = widgetTree(context);
    if (!tree) return out;
    for (uint32_t id = tree->nextMaybeVisible(1); id != 0; id = tree->nextMaybeVisible(id + 1)) {
        const auto* w = tree->get(id);
        if (!w || isRegion(*w) || !w->visible || w->name.empty() || w->name == "UIParent") continue;
        const auto* parent = w->parent != 0 ? tree->get(w->parent) : nullptr;
        if (parent && parent->id != tree->uiParentId() && parent->id != tree->rootId() && parent->name != "UIParent") {
            continue;
        }
        if (w->id == tree->rootId()) continue;
        if (isFurniture(w->name) || w->isTooltip || w->rectW < 20.0f || w->rectH < 20.0f) continue;
        // A frame with nothing in it to see or to press is no window.
        const auto something = [&] {
            for (const uint32_t child : w->children) {
                const auto* c = tree->get(child);
                if (c && c->visible) return true;
            }
            return w->hasBackdrop;
        };
        if (!something()) continue;
        out.push_back(w);
    }
    return out;
}

std::vector<const ui::Widget*> controlsUnder(ToolContext& context, uint32_t root, size_t limit) {
    std::vector<const ui::Widget*> out;
    auto* tree = widgetTree(context);
    if (!tree) return out;
    std::vector<uint32_t> todo{root};
    while (!todo.empty() && out.size() < limit) {
        const uint32_t id = todo.back();
        todo.pop_back();
        const auto* w = tree->get(id);
        if (!w || isRegion(*w) || !w->visibleChain) continue;
        if (w->visible && isControl(*w)) out.push_back(w);
        for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) todo.push_back(*it);
    }
    return out;
}

json popups(ToolContext& context) {
    json out = json::array();
    auto* tree = widgetTree(context);
    if (!tree) return out;
    for (int i = 1; i <= 4; ++i) {
        const std::string name = "StaticPopup" + std::to_string(i);
        const auto* w = tree->findByName(name);
        if (!w || !w->visible) continue;
        json buttons = json::array();
        for (const auto* c : controlsUnder(context, w->id, 12)) buttons.push_back(widgetJson(context, *c));
        json entry = {{"name", name}, {"text", widgetText(context, *w)}, {"controls", std::move(buttons)}};
        // Its which, what click_ui's popup names.
        if (const json which = tryQueryLua(context, "return " + name + ".which"); which.is_string())
            entry["which"] = which;
        out.push_back(std::move(entry));
    }
    return out;
}

json tooltipLines(ToolContext& context, const char* name) {
    auto* tree = widgetTree(context);
    const auto* w = tree ? tree->findByName(name) : nullptr;
    if (!w || !w->visible || w->tooltipLines.empty()) return nullptr;
    json lines = json::array();
    for (const auto& l : w->tooltipLines) {
        std::string text = l.leftVisible() ? plainText(l.left) : std::string();
        if (l.rightVisible() && !l.right.empty()) text += "  //  " + plainText(l.right);
        lines.push_back(std::move(text));
    }
    return lines;
}

}  // namespace wowee::mcp
