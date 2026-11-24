#pragma once

#include <cstddef>
#include <vector>

namespace wowee::addons {

/// What Collapse/ExpandFactionHeader and Collapse/ExpandSkillHeader reach.
/// Each ends in the list's event whether anything changed or not, which is
/// what redraws the window: UPDATE_FACTION (0x005d1880, 0x16a) and
/// SKILL_LINES_CHANGED (0x005cd5e0, 0xf7).
enum class FoldReach { None, One, All };

/// 0x005d1d20: a heading is folded; any other row, or none, folds every
/// top-level group. `index` counts from one.
[[nodiscard]] inline FoldReach factionFold(const std::vector<bool>& rowIsHeader, long index) {
    if (index < 1 || static_cast<size_t>(index) > rowIsHeader.size()) return FoldReach::All;
    return rowIsHeader[static_cast<size_t>(index) - 1] ? FoldReach::One : FoldReach::All;
}

/// 0x005ce3a0: an index below one does every heading ("All"); a row the group
/// it is in, whether it is the heading or not (0x005cd760); past the end
/// nothing.
[[nodiscard]] inline FoldReach skillFold(size_t rows, long index) {
    if (index < 1) return FoldReach::All;
    return static_cast<size_t>(index) <= rows ? FoldReach::One : FoldReach::None;
}

}  // namespace wowee::addons
