#pragma once

// A tooltip's lines as the font strings the interface reads them from.
//
// In the client a GameTooltip's lines ARE font strings: AddLine (0x0061fec0)
// writes the text and colour into <name>TextLeft<n> / <name>TextRight<n> and
// shows them, making the next pair on demand from the font of the one above;
// ClearLines (0x0061c620) sets each used one to "" and hides it. Addons read a
// tooltip that way - `_G[tip:GetName().."TextLeft"..i]:GetText()` for i up to
// NumLines() is how nearly every tooltip scanner works - and FrameXML colours
// a unit's name through TextLeft1:SetTextColor in OnTooltipSetUnit.
//
// This client builds a tooltip's lines in Widget::tooltipLines and draws them
// from there, so the font strings held nothing. These two keep them in step:
// the lines are mirrored into the font strings after a tooltip is filled, and
// a write to one of those font strings is carried back into its line. The
// tooltip goes on drawing its lines itself; a mirrored font string is marked
// (Widget::tooltipLineOwner) so it is not drawn a second time.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace wowee {
namespace ui {

class WidgetTree;
struct Widget;

/// "<tooltip>TextLeft<n>" or "<tooltip>TextRight<n>", n counted from 1
/// (0x0061fec0's "%sTextLeft%d").
std::string tooltipLineStringName(const std::string& tooltip, std::size_t line, bool right);

/// Makes the font string for line `line` (from 1) on one side, returning its
/// widget id, or 0 where it cannot be made. Called only for a name the tree
/// does not already hold.
using MakeTooltipLineString = std::function<uint32_t(std::size_t line, bool right)>;

/// Writes the tooltip's lines into its line font strings: text, colour, and
/// shown where the side has text (0x0061fec0 shows a side only for a
/// non-empty string). The pair after the last line is made too, as the client
/// keeps one spare. Every line font string past the last line is emptied and
/// hidden, as ClearLines leaves it.
///
/// Only a GameTooltip has line font strings: with a name those of its name, and
/// any those it was given (addTooltipLineStrings). Anything else is left alone.
/// Answers how many lines were mirrored.
std::size_t mirrorTooltipLines(WidgetTree& tree, uint32_t tooltipId,
                               const MakeTooltipLineString& make);

/// AddFontStrings (0x006201f0 -> 0x0061fe30): the pair is the next line's that
/// the tooltip has none for - after those of its template, those made for a line
/// and those given before. What is named as that line's is counted only where it
/// is not this pair: a script makes "$parentTextLeft1" and gives it.
void addTooltipLineStrings(WidgetTree& tree, uint32_t tooltipId, uint32_t left, uint32_t right);

/// A line font string was written (SetText, SetTextColor): carry it into the
/// tooltip line it stands for, so the tooltip draws what the interface set.
/// A font string that stands for no line, or for one past NumLines(), changes
/// nothing else. Answers whether a line changed.
bool tooltipLineStringChanged(WidgetTree& tree, const Widget& fontString);

}  // namespace ui
}  // namespace wowee
