#pragma once

// The interface as it is on the screen: the widget tree FrameXML built, as it
// was laid out and drawn.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace wowee::ui { class WidgetTree; struct Widget; }

namespace wowee::mcp {

using json = nlohmann::json;
struct ToolContext;

/// Null while no interface is loaded.
ui::WidgetTree* widgetTree(ToolContext& context);

/// A widget's name, or "#123" for one without.
std::string widgetLabel(const ui::Widget& widget);
/// By its name or "#123"; null for none.
const ui::Widget* widgetNamed(ToolContext& context, const std::string& name);

/// Its rect in window pixels from the top left: x, y, width, height.
glm::vec4 widgetRect(ToolContext& context, const ui::Widget& widget);
/// Whether it answers the mouse: a button, an edit box, a slider.
bool isControl(const ui::Widget& widget);
/// The text on a widget: its own, or of the font strings on it.
std::string widgetText(ToolContext& context, const ui::Widget& widget);

/// Where on it a click would land on it: the middle, or another point of it
/// that nothing covers. Nothing where it is covered or off the screen, with
/// the one in the way in `covering`.
std::optional<glm::vec2> clickPointOf(ToolContext& context, const ui::Widget& widget, std::string* covering = nullptr);
/// Whether a point of the window lands on the widget or on something in it.
bool landsOn(ToolContext& context, const glm::vec2& pixel, const ui::Widget& widget);
/// What the interface has under a point of the window; null for the world.
const ui::Widget* widgetAt(ToolContext& context, const glm::vec2& pixel);

json widgetJson(ToolContext& context, const ui::Widget& widget);

/// The shown windows: what hangs off UIParent and is not part of the screen's
/// furniture (action bars, unit frames, the minimap).
std::vector<const ui::Widget*> openWindows(ToolContext& context);
/// The dialogs that want an answer (StaticPopup1...).
json popups(ToolContext& context);
/// The lines of a tooltip that is shown, or null.
json tooltipLines(ToolContext& context, const char* name);
/// The controls under a widget that can be seen, up to `limit`.
std::vector<const ui::Widget*> controlsUnder(ToolContext& context, uint32_t root, size_t limit);

}  // namespace wowee::mcp
