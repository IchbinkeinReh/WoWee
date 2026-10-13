#pragma once

/// The graphics defaults a fresh install starts at.
///
/// Spelled in three places before: the settings panel's pending fields, the
/// login screen's copy of the same struct, and the reset button's constant. A
/// default that disagrees with itself is a setting that changes when you open
/// a different window.
///
/// They live here rather than in settings_panel.hpp because that header
/// includes vulkan.h, and a test that only wants to know the numbers should
/// not need the Vulkan SDK to find out - which it does not have on macOS.

namespace wowee::ui {

/// The farclip the client's Ultra quality level sets (GraphicsQualityLevels[5],
/// the top of its View Distance slider, 177 to 1277 - FrameXML
/// VideoOptionsPanels.lua). The engine holds whatever is asked for between
/// 183 yards and the map's limit (0x00780770).
inline constexpr float kDefaultViewDistance = 1277.0f;
inline constexpr int   kDefaultGroundClutter = 70;

}  // namespace wowee::ui
