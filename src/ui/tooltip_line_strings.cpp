#include "ui/tooltip_line_strings.hpp"

#include <algorithm>
#include <vector>

#include "ui/widget_tree.hpp"

namespace wowee {
namespace ui {

std::string tooltipLineStringName(const std::string& tooltip, std::size_t line, bool right) {
    return tooltip + (right ? "TextRight" : "TextLeft") + std::to_string(line);
}

namespace {

/// The line font string for line `index` (from 0) on one side: the one this
/// tooltip already knows, else the one by its name, else a new one. 0 where
/// there is none and none can be made.
///
/// The tooltip is looked up again by id each time, because making a font
/// string grows the tree and may move every widget in it.
uint32_t lineString(WidgetTree& tree, uint32_t tooltipId, std::size_t index, bool right,
                    const MakeTooltipLineString& make) {
    Widget* tip = tree.get(tooltipId);
    if (!tip) return 0;
    auto& known = right ? tip->tooltipRightStrings : tip->tooltipLineAnchors;
    if (index < known.size() && known[index] != 0) {
        const Widget* fs = tree.get(known[index]);
        if (fs && fs->kind == WidgetKind::FontString) return known[index];
    }
    if (tip->name.empty()) return 0;
    const std::string name = tooltipLineStringName(tip->name, index + 1, right);
    uint32_t id = 0;
    if (const Widget* found = tree.findByName(name)) {
        if (found->kind == WidgetKind::FontString) id = found->id;
    }
    if (id == 0 && make) id = make(index + 1, right);
    tip = tree.get(tooltipId);
    if (!tip || id == 0) return 0;
    auto& slots = right ? tip->tooltipRightStrings : tip->tooltipLineAnchors;
    if (slots.size() <= index) slots.resize(index + 1, 0);
    slots[index] = id;
    return id;
}

/// One side of a line, written into its font string as AddLine writes it.
void put(WidgetTree& tree, uint32_t id, uint32_t tooltipId, std::size_t index, bool right,
         const std::string& text, const float (&colour)[4], bool shown) {
    Widget* fs = tree.get(id);
    if (!fs || fs->kind != WidgetKind::FontString) return;
    fs->tooltipLineOwner = tooltipId;
    fs->tooltipLineIndex = static_cast<uint32_t>(index);
    fs->tooltipLineRight = right;
    if (fs->text != text) {
        fs->text = text;
        tree.markMeasureDirty(id);
    }
    std::copy(std::begin(colour), std::end(colour), std::begin(fs->color));
    if (fs->shown != shown) {
        fs->shown = shown;
        tree.markVisibilityPending(id);
    }
}

}  // namespace

std::size_t mirrorTooltipLines(WidgetTree& tree, uint32_t tooltipId,
                               const MakeTooltipLineString& make) {
    const Widget* tip = tree.get(tooltipId);
    if (!tip || tip->objectType != "GameTooltip" || (tip->name.empty() && tip->tooltipLineAnchors.empty())) return 0;
    const std::size_t count = tip->tooltipLines.size();

    // The font strings first, made where missing - one pair past the last
    // line, the spare 0x0061fec0 keeps - and only then the writes, so no
    // pointer into the tree is held across a creation.
    std::vector<uint32_t> left(count + 1, 0), right(count + 1, 0);
    for (std::size_t i = 0; i <= count; ++i) {
        left[i] = lineString(tree, tooltipId, i, false, make);
        right[i] = lineString(tree, tooltipId, i, true, make);
    }

    for (std::size_t i = 0; i < count; ++i) {
        tip = tree.get(tooltipId);
        if (!tip || i >= tip->tooltipLines.size()) break;
        // Copied out: put() writes other widgets of the same tree.
        const Widget::TooltipLine line = tip->tooltipLines[i];
        put(tree, left[i], tooltipId, i, false, line.left, line.lc, line.leftVisible());
        put(tree, right[i], tooltipId, i, true, line.right, line.rc, line.rightVisible());
    }

    // Everything past the last line as ClearLines leaves it: no text, hidden.
    tip = tree.get(tooltipId);
    if (!tip) return count;
    const std::vector<uint32_t> knownLeft = tip->tooltipLineAnchors;
    const std::vector<uint32_t> knownRight = tip->tooltipRightStrings;
    for (std::size_t i = count; i < knownLeft.size(); ++i) {
        if (const Widget* fs = tree.get(knownLeft[i]); fs && fs->kind == WidgetKind::FontString)
            put(tree, knownLeft[i], tooltipId, i, false, std::string(), fs->color, false);
    }
    for (std::size_t i = count; i < knownRight.size(); ++i) {
        if (const Widget* fs = tree.get(knownRight[i]); fs && fs->kind == WidgetKind::FontString)
            put(tree, knownRight[i], tooltipId, i, true, std::string(), fs->color, false);
    }
    return count;
}

void addTooltipLineStrings(WidgetTree& tree, uint32_t tooltipId, uint32_t left, uint32_t right) {
    Widget* tip = tree.get(tooltipId);
    if (!tip || left == 0 || right == 0) return;
    std::size_t n = std::max(tip->tooltipLineAnchors.size(), tip->tooltipRightStrings.size());
    while (!tip->name.empty()) {
        const Widget* named = tree.findByName(tooltipLineStringName(tip->name, n + 1, false));
        if (!named || named->kind != WidgetKind::FontString || named->id == left) break;
        ++n;
    }
    tip->tooltipLineAnchors.resize(n + 1, 0);
    tip->tooltipRightStrings.resize(n + 1, 0);
    tip->tooltipLineAnchors[n] = left;
    tip->tooltipRightStrings[n] = right;
}

bool tooltipLineStringChanged(WidgetTree& tree, const Widget& fontString) {
    if (fontString.tooltipLineOwner == 0) return false;
    Widget* tip = tree.get(fontString.tooltipLineOwner);
    if (!tip || !tip->isTooltip) return false;
    const std::size_t i = fontString.tooltipLineIndex;
    if (i >= tip->tooltipLines.size()) return false;
    // Still the font string that line is shown by: a stale owner (the
    // tooltip has since been given another under that name) answers nothing.
    const auto& known = fontString.tooltipLineRight ? tip->tooltipRightStrings : tip->tooltipLineAnchors;
    if (i >= known.size() || known[i] != fontString.id) return false;
    auto& line = tip->tooltipLines[i];
    std::string& text = fontString.tooltipLineRight ? line.right : line.left;
    float (&colour)[4] = fontString.tooltipLineRight ? line.rc : line.lc;
    int8_t& shown = fontString.tooltipLineRight ? line.rightShown : line.leftShown;
    text = fontString.text;
    std::copy(std::begin(fontString.color), std::end(fontString.color), std::begin(colour));
    // Shown or hidden as the font string is now, whatever its text: a side
    // with no text given some is still hidden until it is shown, as in the
    // client, and one hidden by hand stays hidden with its text.
    shown = fontString.shown ? 1 : 0;
    // The sizing pass measures the tooltip from its lines.
    tree.markMeasureDirty(tip->id);
    return true;
}

}  // namespace ui
}  // namespace wowee
