#pragma once

// /gps: where the client has the player, in the server's convention, as a
// line that goes straight back into the GM command `.go xyz X Y Z MAP O`.
// Header-only, no game state: the command hands it the numbers.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace wowee::ui::client_gps {

/// The orientation as the server has it: [0, 2 pi), 0 north, counter-clockwise.
inline float wrapOrientation(float serverYaw) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    float o = std::fmod(serverYaw, kTwoPi);
    if (o < 0.0f) o += kTwoPi;
    if (o >= kTwoPi) o = 0.0f;
    return o;
}

/// `.go xyz X Y Z MAP O`, three decimals for the position (a millimetre in
/// yards is well under what the server rounds to) and four for the facing.
inline std::string goXyzLine(uint32_t mapId, float x, float y, float z, float serverYaw) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), ".go xyz %.3f %.3f %.3f %u %.4f",
                  static_cast<double>(x), static_cast<double>(y), static_cast<double>(z),
                  static_cast<unsigned>(mapId), static_cast<double>(wrapOrientation(serverYaw)));
    return buf;
}

/// The zone line: "Zone: Undercity - Trade Quarter", the subzone left out
/// when there is none or it is the zone again; empty with no zone.
inline std::string zoneLine(const std::string& zone, const std::string& subzone) {
    if (zone.empty() && subzone.empty()) return {};
    std::string out = "Zone: " + (zone.empty() ? subzone : zone);
    if (!zone.empty() && !subzone.empty() && subzone != zone) out += " - " + subzone;
    return out;
}

/// Whether two positions differ enough to show both (the movement the
/// client last sent and where its model stands).
inline bool differs(float ax, float ay, float az, float bx, float by, float bz, float tolerance = 0.05f) {
    return std::fabs(ax - bx) > tolerance || std::fabs(ay - by) > tolerance || std::fabs(az - bz) > tolerance;
}

} // namespace wowee::ui::client_gps
