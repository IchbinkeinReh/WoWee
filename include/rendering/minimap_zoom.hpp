#pragma once

/// The minimap's zoom levels as the client has them (Minimap.cpp).

#include <array>

namespace wowee::rendering::minimap_zoom {

/// Minimap:GetZoomLevels (0x007f3b60): six, 0 furthest out.
constexpr int kLevels = 6;

/// The minimapZoom and minimapInsideZoom cvars' default (registered with "3"
/// in CVar__RegisterAll; 0x00af4e4c and 0x00af4e50 start at 3 too).
constexpr int kDefaultLevel = 3;

/// The outdoor map's width at each level in chunks of 33.33 yards
/// (0x00a41e04): 14, 12, 10, 8, 6, 4.
constexpr std::array<int, kLevels> kOutdoorChunks = {14, 12, 10, 8, 6, 4};

/// The indoor map's radius in yards at each level (0x00a41e1c).
constexpr std::array<float, kLevels> kIndoorRadius = {150.0f, 120.0f, 90.0f, 60.0f, 40.0f, 25.0f};

/// 0x007f3ae0: what Minimap:SetZoom keeps. Taken unsigned, so anything past 4
/// - a negative level included - becomes 5.
constexpr int clampLevel(int level) {
    return static_cast<unsigned>(level) > 4u ? 5 : level;
}

/// 0x007f3b90: how far the map reaches from the player, in yards, at a
/// level: half its width outdoors (233 yards at 0 down to 67 at 5), the
/// table's indoors.
constexpr float radius(int level, bool indoors = false) {
    const int i = clampLevel(level);
    return indoors ? kIndoorRadius[i] : static_cast<float>(kOutdoorChunks[i]) * 0.5f * 33.333332f;
}

}  // namespace wowee::rendering::minimap_zoom
