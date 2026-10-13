#pragma once

// SMSG_CHANGEPLAYER_DIFFICULTY_RESULT as the client reads it (0x0052c460).

#include <cstdint>
#include <optional>
#include <string_view>

namespace wowee::game::difficulty_change {

/// The GlobalStrings error a result shows through the UI error path
/// (0x005216f0), nullptr for a result that shows none. 1 also reads the
/// cooldown's seconds and fills them into the error's %s.
inline const char* errorKey(uint32_t result) {
    switch (result) {
        case 1: return "ERR_DIFFICULTY_CHANGE_COOLDOWN_S";        // 0x2ca
        case 2: return "ERR_DIFFICULTY_CHANGE_WORLDSTATE";        // 0x2cb
        case 3: return "ERR_DIFFICULTY_CHANGE_ENCOUNTER";         // 0x2cc
        case 4: return "ERR_DIFFICULTY_CHANGE_COMBAT";            // 0x2cd
        case 5: return "ERR_DIFFICULTY_CHANGE_PLAYER_BUSY";       // 0x2ce
        case 7: return "ERR_DIFFICULTY_CHANGE_ALREADY_STARTED";   // 0x2cf
        default: return nullptr;
    }
}

/// Map.dbc Flags: a raid whose leader can switch it between normal and
/// heroic (0x00519640, and GetInstanceInfo's isDynamicInstance, 0x0051a8c0).
constexpr uint32_t kMapFlagDynamicDifficulty = 0x100;

/// 0x0052cbc0, CanChangePlayerDifficulty's first answer and the gate of
/// ChangePlayerDifficulty: the current map is a raid (Map.dbc InstanceType 2)
/// that can switch difficulty, the player is in a raid group (0xbeb608 counts
/// its members) and leads it (0xbd1968).
inline bool mayChange(uint32_t mapInstanceType, uint32_t mapFlags, bool inRaid,
                      uint64_t leaderGuid, uint64_t playerGuid) {
    return mapInstanceType == 2 && (mapFlags & kMapFlagDynamicDifficulty) != 0 && inRaid &&
           leaderGuid != 0 && leaderGuid == playerGuid;
}

/// 0x0052d000 and 0x0052d8d0: the cooldown is a wall-clock time in seconds
/// (0xbd1984, time() plus the seconds result 6 sends), 0 for none, running
/// while time() is before it.
inline bool coolingDown(int64_t cooldownEnd, int64_t now) {
    return cooldownEnd != 0 && now - cooldownEnd < 0;
}

/// A time as 0x0061a9e0 words it with "INT_SPELL_DURATION", rounding up
/// (its 7th argument): the GlobalStrings key's suffix and the number for its
/// %d. Under a minute it is whole seconds, rounded down.
struct Duration {
    const char* suffix;  ///< "_SEC", "_MIN", "_HOURS" or "_DAYS"
    uint32_t value;
};

inline Duration intDuration(uint32_t ms) {
    constexpr uint32_t kMinute = 60000, kHour = kMinute * 60, kDay = kMinute * 1440;
    auto ceilDiv = [](uint32_t v, uint32_t d) { return v == 0 ? 0 : (v - 1) / d + 1; };
    if (ms >= kDay) return {"_DAYS", ceilDiv(ms, kDay)};
    if (ms >= kHour) {
        const uint32_t hours = ceilDiv(ms, kHour);
        return hours == 24 ? Duration{"_DAYS", 1} : Duration{"_HOURS", hours};
    }
    if (ms >= kMinute) {
        const uint32_t minutes = ceilDiv(ms, kMinute);
        return minutes == 60 ? Duration{"_HOURS", 1} : Duration{"_MIN", minutes};
    }
    return {"_SEC", ms / 1000};
}

/// enUS GlobalStrings, for a client with no interface strings to read.
inline const char* englishText(const char* key) {
    struct Entry { const char* key; const char* text; };
    static constexpr Entry kText[] = {
        {"ERR_DIFFICULTY_CHANGE_COOLDOWN_S",
         "Raid difficulty has changed recently, and may not change again for %s."},
        {"ERR_DIFFICULTY_CHANGE_WORLDSTATE",
         "Raid difficulty cannot be changed at this time. An event is in progress."},
        {"ERR_DIFFICULTY_CHANGE_ENCOUNTER",
         "Raid difficulty cannot be changed at this time. An encounter is in progress."},
        {"ERR_DIFFICULTY_CHANGE_COMBAT",
         "Raid difficulty cannot be changed at this time. A player is in combat."},
        {"ERR_DIFFICULTY_CHANGE_PLAYER_BUSY",
         "Raid difficulty cannot be changed at this time. A player is busy."},
        {"ERR_DIFFICULTY_CHANGE_ALREADY_STARTED", "A raid difficulty change is currently in progress."},
        {"ERR_PLAYER_DIFFICULTY_CHANGED_S", "Difficulty set to %s."},
        {"PLAYER_DIFFICULTY1", "Normal"},
        {"PLAYER_DIFFICULTY2", "Heroic"},
        {"INT_SPELL_DURATION_SEC", "%d sec"},
        {"INT_SPELL_DURATION_MIN", "%d min"},
        {"INT_SPELL_DURATION_HOURS", "%d |4hour:hrs;"},
        {"INT_SPELL_DURATION_DAYS", "%d |4day:days;"},
    };
    for (const Entry& e : kText) {
        if (std::string_view(e.key) == key) return e.text;
    }
    return "";
}

/// A console line as 0x007658a0 and 0x00768900 split it: spaces before the
/// command skipped, the command up to the first space, its argument the rest
/// with the spaces around it trimmed.
struct ConsoleLine {
    std::string_view command;
    std::string_view args;
};

inline ConsoleLine splitConsoleLine(std::string_view line) {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    const size_t space = line.find(' ');
    ConsoleLine out{line.substr(0, space), {}};
    if (space == std::string_view::npos) return out;
    std::string_view args = line.substr(space);
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    while (!args.empty() && args.back() == ' ') args.remove_suffix(1);
    out.args = args;
    return out;
}

/// The console command changePlayerDifficulty (0x0052d060, registered by
/// 0x0052d0e0 with category 5 and no help text): its argument read as
/// 0x0076f0d0 reads one - an optional '-', then the digits up to the first
/// other character, 0 when there are none - and sent as
/// CMSG_CHANGEPLAYER_DIFFICULTY when under 2, with none of
/// ChangePlayerDifficulty's checks. Nothing for anything else, which the
/// console answers "Invalid difficulty".
inline std::optional<uint32_t> consoleDifficulty(std::string_view args) {
    size_t i = 0;
    const bool negative = !args.empty() && args[0] == '-';
    if (negative) ++i;
    uint32_t value = 0;
    while (i < args.size() && args[i] >= '0' && args[i] <= '9') {
        value = value * 10u + static_cast<uint32_t>(args[i] - '0');
        ++i;
    }
    if (negative) value = 0u - value;
    if (value < 2) return value;
    return std::nullopt;
}

}  // namespace wowee::game::difficulty_change
