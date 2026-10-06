// fullscreen_scale.hpp - The scale SetupFullscreenScale gives a full-screen frame.
#pragma once

#include <algorithm>

namespace wowee {
namespace ui {

/// The scale that makes a frame laid out on a 1024 by 768 guide fill the
/// screen as the client's full-screen frames do, for a screen rootW by rootH
/// interface units.
///
/// The client's SetupFullscreenScale(frame) is frame:SetScale(min(1, aspect *
/// 0.75)). It is called on frames taken out of UIParent (the full-size world
/// map does SetParent(nil) first), which do not take the interface scale, so
/// their screen is always 768 units tall: the guide fills the height and
/// shrinks only on a screen narrower than 4:3. This client's root does take
/// the interface scale and is 768 / scale units tall, so the same picture
/// needs rootH / 768 on top of it - which is min(rootH / 768, rootW / 1024).
///
/// The fit this replaces stopped at 1, so with the interface scale below 1 the
/// map came out that much smaller than the screen; the client's does not.
[[nodiscard]] inline float fullscreenFrameScale(float rootW, float rootH) {
    if (rootW <= 0.0f || rootH <= 0.0f) return 1.0f;
    return std::min(rootH / 768.0f, rootW / 1024.0f);
}

}  // namespace ui
}  // namespace wowee
