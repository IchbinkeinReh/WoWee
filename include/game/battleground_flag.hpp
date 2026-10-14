#pragma once

// Which flag a battleground's flag carrier is drawn holding on the map.
//
// GetBattlefieldFlagPosition's third value (0x0054dcc0) is 0x0054d010's: the
// carrier's team, as 0x006d6e90 has it from its race - its ChrRaces faction
// template's group, 0 for Horde, 1 for Alliance - indexes {"AllianceFlag",
// "HordeFlag"}, so a Horde carrier holds the Alliance flag. A carrier out of
// sight is taken to be on the player's side when it is in the player's raid
// (0x00549670) and on the other one when it is not. On Eye of the Storm (map
// 566) the index is the other way round.

#include <cstdint>
#include <optional>
#include <glm/glm.hpp>

namespace wowee::game::bg_flag {

/// 0x006d6e90: 0 Horde, 1 Alliance, -1 a race with neither. ChrRaces gives
/// Orc, Undead, Tauren, Troll and Blood Elf a Horde template (group 4) and
/// every other race up to 21 an Alliance one (group 2).
inline int raceTeam(uint8_t race) {
    switch (race) {
        case 2: case 5: case 6: case 8: case 10:
            return 0;
        default:
            return race >= 1 && race <= 21 ? 1 : -1;
    }
}

/// 0x0054d010's team for a carrier: its own where it is in sight, else the
/// player's, or the other one where it is not in the player's raid.
inline int carrierTeam(std::optional<uint8_t> carrierRaceInSight, uint8_t playerRace, bool carrierInRaid) {
    if (carrierRaceInSight) return raceTeam(*carrierRaceInSight);
    const int team = raceTeam(playerRace);
    return carrierInRaid ? team : static_cast<int>(team == 0);
}

/// The texture's name under Interface\WorldStateFrame, or none where the
/// client would index past its two.
inline const char* texture(int team, uint32_t mapId) {
    static const char* const kNames[2] = {"AllianceFlag", "HordeFlag"};
    constexpr uint32_t kEyeOfTheStorm = 566;
    const int index = mapId == kEyeOfTheStorm ? static_cast<int>(team == 0) : team;
    return index == 0 || index == 1 ? kNames[index] : nullptr;
}

/// Where a carrier is on the map (0x0054dcc0): its own place where it is in
/// sight (0x005444f0 asks the object for it, vtable +0x2c), else - or where
/// that lies off the map, which 0x005444f0 answers as (0, 0) - the place
/// MSG_BATTLEGROUND_PLAYER_POSITIONS gave it (0x0054a4a0). `project` takes a
/// canonical x, y to the map's u, v, false where it cannot.
template <typename Project>
std::optional<glm::vec2> mapPosition(std::optional<glm::vec2> carrierInSight, const glm::vec2& packet,
                                     Project&& project) {
    float u = 0.0f, v = 0.0f;
    if (carrierInSight && project(carrierInSight->x, carrierInSight->y, u, v) && (u != 0.0f || v != 0.0f))
        return glm::vec2(u, v);
    if (project(packet.x, packet.y, u, v)) return glm::vec2(u, v);
    return std::nullopt;
}

}  // namespace wowee::game::bg_flag
