#pragma once

// SMSG_CHANGEPLAYER_DIFFICULTY_RESULT as the client reads it (0x0052c460).

#include <cstdint>
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

}  // namespace wowee::game::difficulty_change
