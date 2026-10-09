#pragma once

/// The marks the client draws on the minimap: which picture, how big, and
/// where (MinimapFrame.cpp).

#include <array>
#include <cmath>
#include <cstdint>

namespace wowee::rendering::minimap_blips {

/// Minimap.xml's Minimap frame is 140 units square. Every size below is in
/// those units (1024x768 virtual), so a size in pixels is the size times the
/// map's on-screen width over this.
constexpr float kFrameSize = 140.0f;

/// 0x0057dca0 builds the quads from a fraction of the screen's height,
/// 1.6666 * (0.0125, 0.0375, 0.045, 0.055) of it, and the screen is 768 units
/// tall: a blip is 16 units across.
constexpr float kBlipSize = 0.0125f * (5.0f / 3.0f) * 768.0f;
/// The rim arrows (Rotating-MinimapGroupArrow and the corpse and guide
/// arrows): 57.6 units across.
constexpr float kRimArrowSize = 0.045f * (5.0f / 3.0f) * 768.0f;
/// How far from the centre a rim arrow sits: 0.8 of 0.055 of the screen
/// (0x0057d860, 0x00580ae0) - 56 units, against the frame's 70.
constexpr float kRimArrowDistance = 0.8f * 0.055f * (5.0f / 3.0f) * 768.0f;

/// 0x00a11c4c, as 0x0057f7f0 and 0x007f3f40 use it: an object further than
/// 0.8 of the map's reach gets no blip at all, and a party member that far
/// gets a rim arrow instead.
constexpr float kBlipReach = 0.8f;

/// A cell of an atlas, as texture coordinates. v0 is the cell's top row.
struct Cell {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

/// Cell `index` of an atlas cut `columns` x `rows`, counted along the rows.
constexpr Cell gridCell(int index, int columns, int rows) {
    const int column = index % columns;
    const int row = index / columns;
    const float w = 1.0f / static_cast<float>(columns);
    const float h = 1.0f / static_cast<float>(rows);
    return {static_cast<float>(column) * w, static_cast<float>(row) * h,
            static_cast<float>(column + 1) * w, static_cast<float>(row + 1) * h};
}

/// Interface\Minimap\ObjectIcons: eight by two (0x005832f0).
constexpr int kObjectIconColumns = 8;
constexpr int kObjectIconRows = 2;
constexpr Cell objectIconCell(int index) {
    return gridCell(index, kObjectIconColumns, kObjectIconRows);
}

/// Interface\Minimap\PartyRaidBlips: eight by four (0x005832f0).
constexpr int kPartyRaidColumns = 8;
constexpr int kPartyRaidRows = 4;
constexpr Cell partyRaidCell(int index) {
    return gridCell(index, kPartyRaidColumns, kPartyRaidRows);
}

/// The ObjectIcons cells 0x0057f7f0 sorts objects into (the list index is the
/// cell, 0x00580380 draws lists 2 to 15 with cells 2 to 15).
enum ObjectIcon : int {
    kTrackedPlayerOtherTeam = 3,  ///< a tracked player of the other side
    kTrackedPlayerSameTeam = 4,   ///< a tracked player of the player's side
    kTrackedHostile = 5,          ///< a tracked creature, hostile
    kTrackedNeutral = 6,          ///< unfriendly or neutral
    kTrackedFriendly = 7,         ///< friendly or better
    kTrackedResource = 8,         ///< a herb, a vein, treasure, a school of fish
    kQuestAvailable = 9,          ///< the yellow !
    kQuestReward = 10,            ///< the yellow ?
    kQuestAvailableRep = 11,      ///< the blue !
    kOwnMinion = 15,              ///< what the player charms or summoned
};

/// A tracked creature's cell by its reaction to the player, 0 hated to 7
/// exalted: friendly and better 7, unfriendly and neutral 6, the rest 5
/// (0x0057f7f0 on 0x007251c0).
constexpr int trackedCreatureIcon(int reaction) {
    return reaction >= 4 ? kTrackedFriendly : reaction > 1 ? kTrackedNeutral : kTrackedHostile;
}

/// The side a player race is on: 0 the Alliance, 1 the Horde, -1 unknown.
constexpr int raceTeam(uint8_t race) {
    switch (race) {
        case 1: case 3: case 4: case 7: case 11: return 0;
        case 2: case 5: case 6: case 8: case 10: return 1;
        default: return -1;
    }
}

/// Whether a creature type (from 1) is one PLAYER_TRACK_CREATURES has a bit
/// for (0x006dca00).
constexpr bool creatureTypeTracked(uint32_t creatureType, uint32_t trackCreatures) {
    return creatureType >= 1 && creatureType <= 32 &&
           ((trackCreatures >> (creatureType - 1)) & 1u) != 0;
}

/// UNIT_FIELD_BYTES_1's third byte, 0x4: never tracked. And
/// UNIT_DYNAMIC_FLAGS 0x2: tracked whatever its type (0x006dca00).
constexpr uint32_t kUnitUntrackable = 0x4;
constexpr uint32_t kDynamicTrackUnit = 0x2;

/// Whether a unit shows to the player's creature tracking (0x006dca00).
constexpr bool unitTracked(uint32_t bytes1, uint32_t dynamicFlags, uint32_t creatureType,
                           uint32_t trackCreatures) {
    if (((bytes1 >> 16) & kUnitUntrackable) != 0) return false;
    if ((dynamicFlags & kDynamicTrackUnit) != 0) return true;
    return creatureTypeTracked(creatureType, trackCreatures);
}

/// The LockType bits a Lock.dbc row opens to skill: 1 << (Index - 1) for each
/// of its eight slots of Type 2 (0x006dca90).
constexpr uint32_t lockSkillMask(const uint32_t (&types)[8], const uint32_t (&indices)[8]) {
    uint32_t mask = 0;
    for (int i = 0; i < 8; ++i) {
        if (types[i] == 2 && indices[i] >= 1 && indices[i] <= 32) mask |= 1u << (indices[i] - 1);
    }
    return mask;
}

/// Which of a game object's data fields holds its lock, for the two types
/// tracking finds things of: a chest (herbs and veins are chests too) and a
/// fishing hole. -1 for the rest.
constexpr int lockDataIndex(uint32_t gameObjectType) {
    return gameObjectType == 3 ? 0 : gameObjectType == 25 ? 4 : -1;
}

/// The quest-giver status (DIALOG_STATUS_*) to its ObjectIcons cell, or -1
/// for none. 0x0057f7f0: 10 is the ?, 8 the !, 7 the blue !; 2 and 4, the
/// low-level ones, only while the "Low Level Quests" tracking is chosen.
/// Nothing else a quest giver says earns a blip - not 5, the quest still in
/// progress, nor 6 or 9.
constexpr int questGiverIcon(uint8_t status, bool trackingLowLevel) {
    switch (status) {
        case 10: return kQuestReward;
        case 8:  return kQuestAvailable;
        case 2:  return trackingLowLevel ? kQuestAvailable : -1;
        case 7:  return kQuestAvailableRep;
        case 4:  return trackingLowLevel ? kQuestAvailableRep : -1;
        default: return -1;
    }
}

/// A group member's PartyRaidBlips cell (0x0057ff70): the class, from 1, in
/// the top two rows for the player's own party and the bottom two for the
/// rest of the raid. -1 for no class.
constexpr int partyRaidIcon(uint8_t classId, bool ownParty) {
    if (classId == 0) return -1;
    const int cell = ownParty ? classId - 1 : classId + 15;
    return cell < kPartyRaidColumns * kPartyRaidRows ? cell : -1;
}

/// Whether a point `distance` yards away gets a blip on a map reaching
/// `reach` yards.
constexpr bool withinBlipReach(float distance, float reach) {
    return reach > 0.0f && distance <= reach * kBlipReach;
}

/// The texture coordinates of a rim arrow's four corners, in the order the
/// quad's corners are given: bottom-left, bottom-right, top-left, top-right
/// (in frame units, y up). `bearing` is the direction to what it points at,
/// 0 north and growing toward the west - a facing. The art is turned rather
/// than the quad: 0x00580ae0 rotates the coordinates about the middle by the
/// bearing less 3/4 of a turn (0x00acec64).
inline std::array<std::array<float, 2>, 4> rimArrowUVs(float bearing) {
    const float a = bearing - 2.3561945f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    return {{{s + 0.5f, 0.5f - c},
             {0.5f - c, 0.5f - s},
             {c + 0.5f, s + 0.5f},
             {0.5f - s, c + 0.5f}}};
}

/// Where a point of the rim arrow's art lands, in frame units from the
/// arrow's middle with y up, for `u` and `v` across the image (v down it).
///
/// rimArrowUVs gives the quad's corners coordinates up to 0.7 outside the
/// image, which the client clamps to its transparent edge; this draws the art
/// where those coordinates put it instead, so nothing depends on how the
/// sampler treats the outside. The coordinates are a linear map M of the
/// corner's offset over the half size, and M times M is half the identity, so
/// the way back is 2M.
inline std::array<float, 2> rimArrowArtPoint(float bearing, float u, float v) {
    const float a = bearing - 2.3561945f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    const float du = u - 0.5f;
    const float dv = v - 0.5f;
    const float h = kRimArrowSize * 0.5f;
    return {(-(c + s) * du + (c - s) * dv) * h,
            ((c - s) * du + (c + s) * dv) * h};
}

/// Where a rim arrow's middle sits, in frame units from the map's centre with
/// y up: kRimArrowDistance along the bearing (0x00580ae0).
inline std::array<float, 2> rimArrowOffset(float bearing) {
    return {-std::sin(bearing) * kRimArrowDistance, std::cos(bearing) * kRimArrowDistance};
}

}  // namespace wowee::rendering::minimap_blips
