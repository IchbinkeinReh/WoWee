#pragma once

// /gps: where the client has the player, in the server's convention, as a
// line that goes straight back into the GM command `.go xyz X Y Z MAP O`.
// Header-only, no game state: the command hands it the numbers.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

/// The camera as /gps reports it and /cam (WoweeSetCamera) puts it back:
/// its distance from the pivot in yards, its pitch and its yaw from the
/// character's facing in degrees - CameraController::ViewPose, where the
/// camera is going, the yaw 0 behind the character.
struct CameraPose {
    float distance = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
};

/// "Camera: distance 12.40 pitch 20.00 yaw -35.50", which /cam takes back
/// whole or as its three numbers.
inline std::string cameraLine(const CameraPose& c) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Camera: distance %.2f pitch %.2f yaw %.2f",
                  static_cast<double>(c.distance), static_cast<double>(c.pitch), static_cast<double>(c.yaw));
    return buf;
}

/// /cam's argument: three numbers - distance, pitch, yaw - or a /gps camera
/// line with the words; none if it is neither, or a number is not finite or
/// the distance is below zero.
inline std::optional<CameraPose> parseCamera(const std::string& text) {
    std::istringstream in(text);
    std::string word;
    CameraPose c;
    float* next[3] = {&c.distance, &c.pitch, &c.yaw};
    int got = 0;
    while (in >> word && got < 3) {
        if (word == "Camera:" || word == "distance" || word == "pitch" || word == "yaw") continue;
        try {
            size_t used = 0;
            const float v = std::stof(word, &used);
            if (used != word.size() || !std::isfinite(v)) return std::nullopt;
            *next[got++] = v;
        } catch (...) {
            return std::nullopt;
        }
    }
    if (got != 3 || c.distance < 0.0f) return std::nullopt;
    return c;
}

/// The time of day as a clock reads it, "19:30", from hours (wrapped into
/// 0-24 and rounded to the minute).
inline std::string clockText(float hours) {
    float h = std::fmod(hours, 24.0f);
    if (h < 0.0f) h += 24.0f;
    int minutes = static_cast<int>(std::lround(h * 60.0f)) % (24 * 60);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
    return buf;
}

/// "Time: 19:30" - the time of day the lighting is drawn for - and, with the
/// Debug Time override on (`overrideHours` at or over 0), "(Debug Time
/// 19.50)" after it: what /daytime takes back to draw the same light again.
inline std::string timeLine(float lightHours, float overrideHours) {
    std::string out = "Time: " + clockText(lightHours);
    if (overrideHours >= 0.0f) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " (Debug Time %.2f)", static_cast<double>(overrideHours));
        out += buf;
    }
    return out;
}

/// /daytime's argument: hours ("19.5"), a clock ("19:30"),
/// "off" for the server's time again, or a /gps "Time:" line - its Debug
/// Time when it names one, its clock otherwise.
struct TimeArg {
    bool off = false;
    float hours = 0.0f;
};
inline std::optional<TimeArg> parseTime(const std::string& text) {
    auto number = [](const std::string& w, float& out) {
        try {
            size_t used = 0;
            const float v = std::stof(w, &used);
            if (used != w.size() || !std::isfinite(v)) return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    };
    auto clock = [&](const std::string& w, float& out) {
        const size_t colon = w.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= w.size()) return false;
        float h = 0.0f, m = 0.0f;
        if (!number(w.substr(0, colon), h) || !number(w.substr(colon + 1), m)) return false;
        if (m < 0.0f || m >= 60.0f) return false;
        out = h + m / 60.0f;
        return true;
    };
    std::istringstream in(text);
    std::vector<std::string> words;
    for (std::string w; in >> w;) {
        while (!w.empty() && (w.back() == ')' || w.back() == ',')) w.pop_back();
        while (!w.empty() && w.front() == '(') w.erase(w.begin());
        if (!w.empty()) words.push_back(w);
    }
    if (words.empty()) return std::nullopt;
    if (words.size() == 1 && (words[0] == "off" || words[0] == "server")) return TimeArg{true, 0.0f};
    std::optional<float> clockHours, debugHours, plain;
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[i];
        if (w == "Time:" || w == "Debug") continue;
        float v = 0.0f;
        if (w == "Time" && i > 0 && words[i - 1] == "Debug") continue;
        if (i > 0 && words[i - 1] == "Time" && i > 1 && words[i - 2] == "Debug" && number(w, v)) {
            debugHours = v;
        } else if (clock(w, v)) {
            clockHours = v;
        } else if (number(w, v)) {
            plain = v;
        } else {
            return std::nullopt;
        }
    }
    const std::optional<float> h = debugHours ? debugHours : clockHours ? clockHours : plain;
    if (!h || *h < 0.0f || *h > 24.0f) return std::nullopt;
    return TimeArg{false, *h};
}

} // namespace wowee::ui::client_gps
