#pragma once

/// The lines the world state frame shows, as the client builds them from
/// WorldStateUI.dbc (WorldStateUI.cpp 0x005487d0-0x00548d40): the rows for
/// the map, zone and area SMSG_INIT_WORLD_STATES names and the phases
/// SMSG_SET_PHASE_SHIFT gives, each line's text with the world states it
/// names put in (0x005770d0).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::game::world_state_ui {

/// A WorldStateUI.dbc row as the client keeps it (0x3c bytes).
struct Row {
    uint32_t id = 0;
    int32_t mapId = -1;
    uint32_t areaId = 0;
    uint32_t phaseShift = 0;
    std::string icon;
    std::string text;
    std::string tooltip;
    uint32_t stateVariable = 0;
    uint32_t type = 0;
    std::string dynamicIcon;
    std::string dynamicTooltip;
    std::string extendedUI;
    uint32_t extendedUIState[3] = {0, 0, 0};
};

/// Where the world states are for (0xbe8f54, 0xbe8f50, 0xbe8f4c) and the
/// player's phases (0xac80a4).
struct Place {
    int32_t mapId = -1;
    uint32_t zoneId = 0;
    uint32_t areaId = 0;
    uint32_t phaseMask = 1;
    bool operator==(const Place&) const = default;
};

/// 0x005487d0: the rows shown there - any map's or this one's, any area's
/// or the zone's or the area's, any phase's or one of the player's, and of
/// type 0, 1 or 3 - in the file's order.
inline std::vector<size_t> shownRows(const std::vector<Row>& rows, const Place& place) {
    std::vector<size_t> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r.mapId != -1 && r.mapId != place.mapId) continue;
        if (r.areaId != 0 && r.areaId != place.zoneId && r.areaId != place.areaId) continue;
        if (r.phaseShift != 0 && (r.phaseShift & place.phaseMask) == 0) continue;
        if (r.type != 0 && r.type != 1 && r.type != 3) continue;
        out.push_back(i);
    }
    return out;
}

/// A world state's value, 0 for one not known (0x00548d10).
using StateValue = std::function<uint32_t(uint32_t)>;

/// 0x00576e50: a world state holding when something ends, as the time left
/// from `now` - "%02d:%02d" under an hour, "%d:%02d:%02d" from one.
inline std::string timeLeft(int64_t endsAt, int64_t now) {
    int64_t left = endsAt - now;
    if (left < 0) left = 0;
    char buf[32];
    if (left / 3600 < 1) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>((left % 3600) / 60),
                      static_cast<int>((left % 3600) % 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", static_cast<int>(left / 3600),
                      static_cast<int>((left % 3600) / 60), static_cast<int>((left % 3600) % 60));
    }
    return buf;
}

/// 0x005770d0: "%<n>w" the world state n's value, "%<n>k" the time left to
/// the moment world state n holds; any other '%' stays itself.
inline std::string text(const std::string& source, const StateValue& state, int64_t now) {
    std::string out;
    size_t at = 0;
    while (true) {
        const size_t pct = source.find('%', at);
        if (pct == std::string::npos) break;
        out.append(source, at, pct - at);
        size_t p = pct + 1;
        while (p < source.size() && source[p] >= '0' && source[p] <= '9') ++p;
        const uint32_t id = static_cast<uint32_t>(std::strtoul(source.c_str() + pct + 1, nullptr, 10));
        const char kind = p < source.size() ? source[p] : '\0';
        if (kind == 'w' || kind == 'W') {
            out += std::to_string(static_cast<int32_t>(state ? state(id) : 0));
            ++p;
        } else if (kind == 'k' || kind == 'K') {
            out += timeLeft(static_cast<int64_t>(state ? state(id) : 0), now);
            ++p;
        } else {
            out += '%';
        }
        at = p;
    }
    out.append(source, at, std::string::npos);
    return out;
}

}  // namespace wowee::game::world_state_ui
