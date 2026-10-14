#pragma once

// The world's ambience, as the client picks it.
//
// The client keeps eleven sound slots (0xb4aab0, nine words each), each with
// a day and a night ambience sound - SoundEntries ids, -1 for none - among
// other things. Five of them carry ambience:
//   0  the place: AreaTable's or WMOAreaTable's SoundAmbience (0x0078e9a0);
//   1  a world state's: WorldStateZoneSounds.dbc (0x004cbe70);
//   8  the weather: Weather.dbc's AmbienceID on SMSG_WEATHER (0x00526ade ->
//      0x004c8610);
//   9  under liquid: SoundEntries 4209, "Underwater", while the camera is in
//      any (0x005fe7b0 -> 0x004c8630);
//  10  the screen effect: ScreenEffect.dbc's SoundAmbience (0x004c8fa0).
// Every frame the highest slot with a sound for the time of day plays
// (0x004c9910 -> 0x004c86f0); the weather's is passed over indoors. A slot
// filled from a SoundAmbience row takes the row's +4 by day and +8 by night
// (0x004c8c90); a row that is not there leaves the slot empty.
//
// It plays on one channel pair (0xb4ac50, 0x004c7140), as a loop on the
// listener: a new sound rises over five seconds while the one before falls
// over five; one that comes as the camera enters or leaves liquid cuts both
// at once and starts at full. Nothing left to play stops what plays at once.
//
// The one other ambient sound is the liquid's (0x004d2390), below.
//
// The client has nothing else: no birds or crickets of its own (they are in
// the SoundAmbience files), no bells (a capital's hourly bell is the
// server's SMSG_PLAY_SOUND - 0x19c2 and its kin appear nowhere in wow.exe),
// and no SoundEmitters.dbc sounds - it loads that table (0x0064d3e0 into
// 0xad4694) and reads nothing from it.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::audio::zone_ambience {

inline constexpr int kSlotCount = 11;
inline constexpr int kSlotPlace = 0, kSlotWorldState = 1, kSlotWeather = 8, kSlotUnderwater = 9,
                     kSlotScreenEffect = 10;

/// 0x004c8630: (liquid != 0 & 0x1072) - 1.
inline constexpr int32_t kUnderwaterSound = 4209;

/// The rise and the fall of a change of sound (0x004c9910: 5.0 for both).
inline constexpr float kCrossfadeSeconds = 5.0f;

/// A slot's day and night sound, -1 for none. 0 is a sound that plays
/// nothing: it holds the lower slots back all the same.
struct SlotSound {
    int32_t day = -1;
    int32_t night = -1;
    constexpr int32_t at(bool isDay) const { return isDay ? day : night; }
};
using Slots = std::array<SlotSound, kSlotCount>;

/// What plays: the slot and its sound.
struct Pick {
    int slot = 0;
    int32_t sound = -1;
    constexpr bool operator==(const Pick&) const = default;
};

/// 0x004c86f0: from slot 10 down, the first with a sound for the time,
/// passing over the weather's (8) indoors (0x004c8600's flag). None when
/// every slot is empty.
constexpr std::optional<Pick> pick(const Slots& slots, bool isDay, bool indoors) {
    for (int slot = kSlotCount - 1; slot >= 0; --slot) {
        const int32_t sound = slots[static_cast<size_t>(slot)].at(isDay);
        if (sound < 0 || (slot == kSlotWeather && indoors)) continue;
        return Pick{slot, sound};
    }
    return std::nullopt;
}

/// 0x0078e9a0: the SoundAmbience row of a place - the building group's
/// WMOAreaTable row, then the building's own (group -1), then, outside only,
/// the area's AreaTable row and its zone's. 0 for none. A row not there
/// counts as 0.
constexpr uint32_t placeAmbience(uint32_t zone, uint32_t area, uint32_t buildingRow, uint32_t groupRow,
                                 bool indoors) {
    if (groupRow != 0) return groupRow;
    if (buildingRow != 0) return buildingRow;
    if (indoors) return 0;
    return area != 0 ? area : zone;
}

/// A WorldStateZoneSounds.dbc row (0x004cc1b0): +0 the world state, +4 the
/// value it wants, +8 an AreaTable id, +0xc a WMOAreaTable id, +0x18 the
/// SoundAmbience row.
struct WorldStateSound {
    uint32_t worldState = 0;
    uint32_t value = 0;
    uint32_t area = 0;
    uint32_t wmoArea = 0;
    uint32_t ambience = 0;
};

/// Where the player is, as the ambience asks (0x0078f020): the zone and
/// the area its place name is from (0x0077fa00) - the area 0 where it is a
/// zone - and, linked to a building's group (0x007a1640), the group's and
/// the building's own WMOAreaTable rows - their ids and SoundAmbience, 0 for
/// none - and whether the group is inside (0x007a1480).
struct Place {
    uint32_t zone = 0;
    uint32_t area = 0;
    uint32_t buildingRow = 0;
    uint32_t groupRow = 0;
    uint32_t buildingAmbience = 0;
    uint32_t groupAmbience = 0;
    bool indoors = false;
    constexpr bool operator==(const Place&) const = default;
};

/// 0x004cbe70: the row whose world state has its value (0 for a state never
/// sent, 0x00548d10) and which names the place; its SoundAmbience goes to
/// slot 1, none emptying it.
///   Inside: a row for the building or the group, the group's ending it.
///   Outside with a building: the best of the group (first), the area, the
///     zone - the building's own row is not asked.
///   Outside: a row for the zone or the area, in order, the area's ending
///     it.
inline std::optional<uint32_t> worldStateAmbience(const std::vector<WorldStateSound>& rows,
                                                  const Place& at,
                                                  const std::function<uint32_t(uint32_t)>& stateValue) {
    std::optional<uint32_t> found;
    const auto matches = [&](const WorldStateSound& r) { return stateValue(r.worldState) == r.value; };
    if (at.indoors) {
        for (const auto& r : rows) {
            const bool names = (at.buildingRow > 0 && r.wmoArea == at.buildingRow) ||
                               (at.groupRow > 0 && r.wmoArea == at.groupRow);
            if (!names || !matches(r)) continue;
            found = r.ambience;
            if (r.wmoArea == at.groupRow) return found;
        }
        return found;
    }
    if (at.buildingRow > 0 || at.groupRow > 0) {
        unsigned best = 4;
        for (const auto& r : rows) {
            unsigned rank = 4;
            if (at.zone > 0 && r.area == at.zone) rank = 3;
            if (at.area > 0 && r.area == at.area) rank = 2;
            if (at.groupRow > 0 && r.wmoArea == at.groupRow) rank = 0;
            if (rank >= best || !matches(r)) continue;
            found = r.ambience;
            best = rank;
            if (rank == 0) return found;
        }
        return found;
    }
    for (const auto& r : rows) {
        const bool names = (at.zone > 0 && r.area == at.zone) || (at.area > 0 && r.area == at.area);
        if (!names || !matches(r)) continue;
        found = r.ambience;
        if (r.area == at.area) return found;
    }
    return found;
}

// ---------------------------------------------------------------------------
// The liquid's sound (0x004d2390, every frame).
//
// Not while the camera is in liquid (0x004c8670): then nothing is asked and
// what plays goes on. Otherwise at most every half second, and only once the
// player has moved (0x004d2230), the liquid within 60 yards of the player
// is asked for (0x0077f390): every liquid cell whose middle - its centre at
// half way between the liquid's lowest and highest - is within 60 yards
// (0x0079c360). A liquid whose LiquidType has flag 0x80 is passed over with
// the player indoors. Of what is left, the cell nearest across the ground
// wins (0x004d21b0), and its LiquidType's SoundID plays there: a 3D loop
// rising over a second, at the sound's own volume and distances. While the
// same sound wins within 20 yards of where it plays it moves there; a
// different one, or one further off, fades the last out over a second and
// starts anew. With nothing within 60 yards the sound wanted is none
// (sound 0), and what plays fades out.

inline constexpr float kLiquidQueryRadius = 60.0f;
inline constexpr float kLiquidRecheckSeconds = 0.5f;
inline constexpr float kLiquidFollowDistance = 20.0f;
inline constexpr float kLiquidFadeSeconds = 1.0f;
inline constexpr uint32_t kLiquidTypeQuietIndoors = 0x80;

/// One liquid cell as the query finds it: its LiquidType row and where.
struct LiquidCell {
    uint32_t liquidType = 0;
    glm::vec3 position{0.0f};
};

/// Every liquid cell within a radius of a point (0x0077f390).
using LiquidQuery = std::function<std::vector<LiquidCell>(const glm::vec3&, float)>;

/// The cell whose sound plays (0x004d21b0): the nearest across the ground,
/// the first of equals.
inline std::optional<LiquidCell> nearestLiquid(const std::vector<LiquidCell>& cells, const glm::vec3& player) {
    std::optional<LiquidCell> best;
    float bestDist = 0.0f;
    for (const auto& c : cells) {
        const glm::vec2 d(c.position.x - player.x, c.position.y - player.y);
        const float dist = glm::dot(d, d);
        if (best && bestDist <= dist) continue;
        best = c;
        bestDist = dist;
    }
    return best;
}

/// What becomes of the playing liquid sound when a sound is wanted at a
/// place (0x004d2390). Sound 0 is none.
enum class LiquidChange { Keep, Move, Restart };
constexpr LiquidChange liquidChange(int32_t playingSound, const glm::vec3& playingAt, int32_t wantedSound,
                                    const glm::vec3& wantedAt) {
    if (playingSound != wantedSound) return LiquidChange::Restart;
    if (playingAt == wantedAt) return LiquidChange::Keep;
    const glm::vec3 d = playingAt - wantedAt;
    return glm::dot(d, d) < kLiquidFollowDistance * kLiquidFollowDistance ? LiquidChange::Move
                                                                         : LiquidChange::Restart;
}

}  // namespace wowee::audio::zone_ambience
