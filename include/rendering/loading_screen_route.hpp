#pragma once

/// The loading screen of a transport's crossing to another map: the world
/// map with the leg it sails or flies drawn on it, from the stop it left to
/// the one it makes for, its dots coming in as the load goes on.
///
/// SMSG_TRANSFER_PENDING with a transport (its entry and the map it leaves)
/// leads to 0x0040ad50, which takes the player's transport's TaxiPath
/// (its game object's data0), the leg starting on the map it leaves
/// (0x00408cd0) and builds the route (0x00409880):
///
///  - the leg's ends are its stops, the path's TaxiPathNode rows flagged 2,
///    from the one on the map left to the next (0x00408c80);
///  - each end is put on the world map by its continent's WorldMapContinent
///    row (0x005440e0, worldMapPoint);
///  - between them, the leg's LoadingScreenTaxiSplines points (matched by
///    path and leg, 0x00408bb0), up to eight, already in world map space;
///  - a spline through them all, the ends doubled (0x004c3830), dotted at
///    66.67 dots to the map's width (0x00409880, 0x004089c0).
///
/// It is drawn on the twelve World%d tiles of the world map (0x0040a160,
/// 0x00409010) in the 4:3 viewport, the start and end marks and the dots
/// from Interface\Glues\LoadingScreens\DynamicElements (0x00408690,
/// 0x00408e10), and a dot shows once the load has passed its share of the
/// way (0x00408e10's loop against the progress at 0x00b2feac).

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::loading_route {

/// A continent's place on the world map (WorldMapContinent's offset and
/// scale).
struct Continent {
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float scale = 1.0f;
};

/// 0x005440e0: a position on a continent, as a point on the world map,
/// 0..1 across its 1002 by 668 pixels, y up.
inline glm::vec2 worldMapPoint(const Continent& c, float x, float y) {
    const float u = (c.offsetX * 0.015968064f + 0.5f) - y * 2.994012e-05f * c.scale;
    const float v = 1.0f - ((c.offsetY * 0.023952097f + 0.5f) - x * 4.4910183e-05f * c.scale);
    return {u, v};
}

/// 0x00409880: the spline's points - the start twice, the leg's points,
/// the end twice. A point at (0, 0) ends the leg's list.
inline std::vector<glm::vec2> controlPoints(const glm::vec2& start, const std::vector<glm::vec2>& leg,
                                            const glm::vec2& end) {
    std::vector<glm::vec2> cps{start, start};
    for (const glm::vec2& p : leg) {
        if (p.x <= 0.0f && p.y <= 0.0f) break;
        cps.push_back(p);
    }
    cps.push_back(end);
    cps.push_back(end);
    return cps;
}

/// A point on the Catmull-Rom spline through `cps` (the first and last are
/// only its tangents), `t` 0..1 evenly over its segments.
inline glm::vec2 evaluate(const std::vector<glm::vec2>& cps, float t) {
    if (cps.size() < 4) return cps.empty() ? glm::vec2(0.0f) : cps.front();
    const int segments = static_cast<int>(cps.size()) - 3;
    const float s = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(segments);
    const int i = std::min(static_cast<int>(s), segments - 1);
    const float f = s - static_cast<float>(i);
    const glm::vec2 &p0 = cps[i], &p1 = cps[i + 1], &p2 = cps[i + 2], &p3 = cps[i + 3];
    const float f2 = f * f, f3 = f2 * f;
    return 0.5f * ((2.0f * p1) + (p2 - p0) * f + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2 +
                   (3.0f * p1 - p0 - 3.0f * p2 + p3) * f3);
}

/// The spline's length on the world map.
inline float length(const std::vector<glm::vec2>& cps) {
    constexpr int kSteps = 256;
    float total = 0.0f;
    glm::vec2 last = evaluate(cps, 0.0f);
    for (int k = 1; k <= kSteps; ++k) {
        const glm::vec2 p = evaluate(cps, static_cast<float>(k) / kSteps);
        total += glm::length(p - last);
        last = p;
    }
    return total;
}

/// 0x00409880: how many dots - 66.67 to the map's width, one at the least -
/// and so how many segments between them; the dots are that plus one.
inline int dotSegments(float routeLength) {
    return std::max(1, static_cast<int>(std::lround(routeLength * 66.66667f)));
}

/// 0x00408e10: the dots shown at a load's progress, 0..1: the i-th of n
/// once the progress is past i/(n-1).
inline int dotsShown(int dots, float progress) {
    if (dots <= 1) return dots;
    int shown = 0;
    for (int i = 0; i < dots; ++i) {
        if (progress < static_cast<float>(i) / static_cast<float>(dots - 1)) break;
        shown = i + 1;
    }
    return shown;
}

/// Its size on the screen's 0..1 viewport: the marks 0.04 by 0.053, a dot
/// 0.02 by 0.03 (0x00408690, 0xab6420).
inline constexpr glm::vec2 kMarkHalf{0.02f, 0.0265f};
inline constexpr glm::vec2 kDotHalf{0.01f, 0.015f};

/// The world map's tiles: twelve of 256 pixels, four across and three down,
/// over a map of 1002 by 668 (World1 top left).
inline constexpr int kTiles = 12;
inline constexpr float kTileU = 256.0f / 1002.0f;
inline constexpr float kTileV = 256.0f / 668.0f;

}  // namespace wowee::rendering::loading_route
