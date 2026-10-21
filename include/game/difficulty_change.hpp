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

/// The raid difficulty as 0x005255a0 keeps it: the player's own (0xbd089c)
/// and its group's (0xbd197c). Which one is in force depends on whether the
/// player is in a group at all (0xbeb608, the group's member count) -
/// GetRaidDifficulty (0x00515810) answers that one first and the player's own
/// second.
struct RaidDifficulty {
    uint32_t personal = 0;
    uint32_t group = 0;
    [[nodiscard]] uint32_t effective(bool inGroup) const { return inGroup ? group : personal; }
};

/// What MSG_SET_RAID_DIFFICULTY's three words do to it (0x005255a0): the
/// difficulty, then whether it is the player's own and whether it is the
/// group's. The group's is not stored on a map that can switch difficulty in
/// place; there the player's difficulty is asked to follow it instead -
/// heroic for anything past 25 normal (0x0052d8d0 with difficulty > 1).
struct RaidDifficultyApplied {
    bool changed = false;                ///< the one in force differs: 0x00524780 says so
    std::optional<uint32_t> askPlayer;   ///< CMSG_CHANGEPLAYER_DIFFICULTY with this
};

inline RaidDifficultyApplied applyRaidDifficulty(RaidDifficulty& r, uint32_t difficulty, bool own, bool group,
                                                 bool dynamicMap, bool inGroup) {
    RaidDifficultyApplied out;
    const uint32_t before = r.effective(inGroup);
    if (own) r.personal = difficulty;
    if (group) {
        if (!dynamicMap) r.group = difficulty;
        else out.askPlayer = difficulty > 1 ? 1u : 0u;
    }
    out.changed = r.effective(inGroup) != before;
    return out;
}

/// The dungeon difficulty as 0x00525530 keeps it: the player's own
/// (0xbd0898) and the party's (0xbd1978). The party's is in force only in a
/// party - one with its first slot filled (0xbd1948) and no raid roster
/// (0xbeb608); a raid, or no party, has the player's own (GetDungeonDifficulty,
/// 0x00515790). Apart from the instance's difficulty, which only
/// SMSG_INSTANCE_DIFFICULTY sets (0xbd0894).
struct DungeonDifficulty {
    uint32_t personal = 0;
    uint32_t group = 0;
    [[nodiscard]] uint32_t effective(bool inParty) const { return inParty ? group : personal; }
};

/// What MSG_SET_DUNGEON_DIFFICULTY's three words do to it (0x00525530): the
/// difficulty, then whether it is the player's own and whether it is the
/// party's. True when the one in force changed, which 0x00524720 says as
/// ERR_DUNGEON_DIFFICULTY_CHANGED_S with DUNGEON_DIFFICULTY<n>.
inline bool applyDungeonDifficulty(DungeonDifficulty& d, uint32_t difficulty, bool own, bool group, bool inParty) {
    const uint32_t before = d.effective(inParty);
    if (own) d.personal = difficulty;
    if (group) d.group = difficulty;
    return d.effective(inParty) != before;
}

/// The minimap's difficulty flag as MiniMapInstanceDifficulty_OnEvent draws
/// it from GetInstanceInfo (Minimap.lua): shown in a dungeon or a raid that is
/// not a normal five, heroic for a dungeon's difficulty 2, a dynamic raid the
/// player has set to heroic, or a raid past difficulty 2.
struct InstanceBanner {
    bool shown = false;
    bool heroic = false;
};

inline InstanceBanner instanceBanner(std::string_view instanceType, uint32_t difficulty, uint32_t maxPlayers,
                                     uint32_t playerDifficulty, bool dynamicInstance) {
    InstanceBanner b;
    const bool party = instanceType == "party";
    if ((!party && instanceType != "raid") || (difficulty == 1 && maxPlayers == 5)) return b;
    b.shown = true;
    if (party) b.heroic = difficulty == 2;
    else if (dynamicInstance) b.heroic = playerDifficulty == 1;
    else b.heroic = difficulty > 2;
    return b;
}

/// How 0x00524780 words a raid difficulty that changed.
enum class RaidChangeWords : uint8_t {
    Lockout,         ///< on a dynamic-difficulty map: ERR_RAID_LOCKOUT_CHANGED_S with RAID_DIFFICULTY<n>,
                     ///< then ERR_PLAYER_DIFFICULTY_CHANGED_S with PLAYER_DIFFICULTY<n>
    MapDifficulty,   ///< in a raid (InstanceType 2): ERR_RAID_DIFFICULTY_CHANGED_S with the
                     ///< MapDifficulty.dbc row's difficulty string for the instance's difficulty
    RaidDifficulty,  ///< anywhere else: ERR_RAID_DIFFICULTY_CHANGED_S with RAID_DIFFICULTY<n>
};

inline RaidChangeWords raidChangeWords(bool dynamicMap, int mapInstanceType) {
    if (dynamicMap) return RaidChangeWords::Lockout;
    return mapInstanceType == 2 ? RaidChangeWords::MapDifficulty : RaidChangeWords::RaidDifficulty;
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
