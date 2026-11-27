// fullscreen_scale.hpp - The scale SetupFullscreenScale gives a full-screen frame.
#pragma once

#include <algorithm>

namespace wowee {
namespace ui {

/// The scale SetupFullscreenScale gives a frame laid out on a 1024 by 768
/// guide, for a screen rootW by rootH interface units.
///
/// The client's (0x0054ed80) is frame:SetScale(min(1, aspect * 0.75)), the
/// aspect being width / height (0x0047bf90 keeps aspect * 0.75 for it). Its
/// callers take the frame out of UIParent first (the full-size world map's
/// SetParent(nil)), and a frame without a parent does not take the interface
/// scale: its screen is 768 units high here as in the client (widget_tree's
/// trueScale), so the guide fills the height and shrinks only on a screen
/// narrower than 4:3. The interface scale is in rootW and rootH alike and
/// drops out of the aspect.
///
/// It was min(rootH / 768, rootW / 1024) from when a frame without a parent
/// still took the interface scale; since it does not, that put the scale in
/// twice and the map came out 1 / scale too tall (T448).
[[nodiscard]] inline float fullscreenFrameScale(float rootW, float rootH) {
    if (rootW <= 0.0f || rootH <= 0.0f) return 1.0f;
    return std::min(1.0f, rootW / rootH * 0.75f);
}

}  // namespace ui
}  // namespace wowee
