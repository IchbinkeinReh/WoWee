#pragma once

/// The marks the client draws on the minimap: which picture, how big, and
/// where (MinimapFrame.cpp).

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

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
    kTrackedPlayerAttackable = 2, ///< a tracked player the player may attack (0x00729740)
    kTrackedPlayerOtherTeam = 3,  ///< a tracked player of the other side
    kTrackedPlayerSameTeam = 4,   ///< a tracked player of the player's side
    kTrackedHostile = 5,          ///< a tracked creature, hostile
    kTrackedNeutral = 6,          ///< unfriendly or neutral
    kTrackedFriendly = 7,         ///< friendly or better
    kTrackedResource = 8,         ///< a herb, a vein, treasure, a school of fish;
                                  ///< and what the menu's town tracking finds
    kQuestAvailable = 9,          ///< the yellow !
    kQuestReward = 10,            ///< the yellow ?
    kQuestAvailableRep = 11,      ///< the blue !
    kTaxiUnknown = 13,            ///< a flight master whose node the player lacks
    kOwnMinion = 15,              ///< what the player charms or summoned
};
// Cells 12 and 14 are filled by nothing in this client: 0x0057f7f0 sorts
// into 2 to 11, 13 and 15, and no other code writes those two lists.

/// The colour 0x00580380 draws a blip in that stands in another indoor area
/// than the player's (0x0057f7f0's +0x18): grey, 0xb0 of white.
constexpr uint8_t kOtherAreaGrey = 0xb0;

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

/// Interface\Minimap\POIIcons: 196 icons of 18 pixels, as many to a row as
/// the square root of that (0x005832f0) - 14 - each a pixel in from its top
/// left. AreaPOI's icons, a gossip point's and the corpse's are cells of it.
constexpr int kPoiIconCount = 196;
constexpr int kPoiIconColumns = 14;
constexpr float kPoiIconPixels = 18.0f;
/// The corpse's icon (0x007f4990).
constexpr int kCorpsePoiIcon = 8;
/// A point of interest further than this many yards gets no rim arrow - the
/// corpse excepted (0x007f44a0).
constexpr float kPoiArrowReach = 694.44446f;

/// POIIcons cell `icon` of a texture `textureWidth` pixels wide (the client
/// divides both ways by the width).
constexpr Cell poiIconCell(int icon, int textureWidth) {
    const float w = textureWidth > 0 ? static_cast<float>(textureWidth) : 256.0f;
    const float px = 1.0f / w;
    const float cell = kPoiIconPixels / w;
    const float column = static_cast<float>(icon % kPoiIconColumns);
    const float row = static_cast<float>(icon / kPoiIconColumns);
    return {column * cell + px, row * cell + px, (column + 1.0f) * cell, (row + 1.0f) * cell};
}

/// A point of interest's flags (AreaPOI.dbc's, a gossip point's, the
/// corpse's 2): 0x1 an AreaPOI the minimap takes at all (0x007f6730), 0x2 its
/// icon drawn within the blip reach (0x007f44a0), 0x100 its rim arrow kept
/// indoors (0x007f4b60).
constexpr uint32_t kPoiOnMinimap = 0x1;
constexpr uint32_t kPoiShowIcon = 0x2;
constexpr uint32_t kPoiArrowIndoors = 0x100;

/// An AreaPOI's icon (0x00581e80): with a world state, Icon[value - 1] for a
/// value of 1 to 9 and Icon[0] for any other; without one, Icon[0]. -1 while
/// its world state is 0 or unknown, when 0x007f44a0 shows nothing of it.
inline int areaPoiIcon(const std::array<int32_t, 9>& icons, uint32_t worldStateId,
                       const uint32_t* worldStateValue) {
    if (worldStateId == 0) return icons[0];
    if (!worldStateValue || *worldStateValue == 0) return -1;
    const uint32_t i = *worldStateValue - 1;
    return icons[i <= 8 ? i : 0];
}

/// A point beyond the blip reach, as 0x007f44a0 weighs it for a rim arrow.
struct PoiCandidate {
    int32_t importance = 0;  ///< lower first; the corpse is -1
    float distance = 0.0f;   ///< yards
    bool anyDistance = false;  ///< the corpse: kept however far
};

/// Which points get the rim arrows (0x007f44a0): up to three, out to
/// kPoiArrowReach unless anyDistance, the lowest importance first and the
/// nearest of equal importance; on a tie, the earlier. Indices into `c`.
inline std::vector<size_t> chooseRimArrows(const std::vector<PoiCandidate>& c) {
    std::vector<size_t> eligible;
    for (size_t i = 0; i < c.size(); ++i) {
        if (c[i].anyDistance || c[i].distance <= kPoiArrowReach) eligible.push_back(i);
    }
    std::stable_sort(eligible.begin(), eligible.end(), [&](size_t a, size_t b) {
        if (c[a].importance != c[b].importance) return c[a].importance < c[b].importance;
        return c[a].distance < c[b].distance;
    });
    if (eligible.size() > 3) eligible.resize(3);
    return eligible;
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

/// What 0x0057f7f0 asks of a unit, in the order it asks.
struct UnitFacts {
    /// UNIT_FIELD_CHARMEDBY is the player, or with no charmer
    /// UNIT_FIELD_SUMMONEDBY is.
    bool ownMinion = false;
    bool dead = false;          ///< UNIT_FIELD_HEALTH below 1
    /// The creature's type flags have 0x80, interactable dead (0x00715e30):
    /// a dead one is still marked.
    bool deadInteract = false;
    /// The tracking menu's town entry finds it: one of its UNIT_NPC_FLAGS,
    /// and not hostile (0x0071acf0, 0x00729530).
    bool otherTracked = false;
    uint8_t questStatus = 0;    ///< DIALOG_STATUS_*, 0 none
    /// A flight master whose node the player has not learned:
    /// SMSG_TAXINODE_STATUS said 0 (0x006d5fc0 sets the unit's +0x94).
    bool taxiUnknown = false;
    bool tracked = false;       ///< unitTracked (0x006dca00)
    bool isPlayer = false;
    bool attackable = false;    ///< the player may attack it (0x00729740)
    bool sameTeam = false;      ///< its race's side is the player's (0x006d6e90)
    int reaction = 0;           ///< 0 hated .. 7 exalted (0x007251c0)
};

/// The unit's ObjectIcons cell, or -1 for no blip (0x0057f7f0).
constexpr int unitIcon(const UnitFacts& f, bool trackingLowLevel) {
    if (f.ownMinion) return kOwnMinion;
    if (f.dead && !f.deadInteract) return -1;
    if (f.otherTracked) return kTrackedResource;
    const int quest = questGiverIcon(f.questStatus, trackingLowLevel);
    if (quest >= 0) return quest;
    if (f.taxiUnknown) return kTaxiUnknown;
    if (!f.tracked) return -1;
    if (f.isPlayer) {
        if (f.attackable) return kTrackedPlayerAttackable;
        return f.sameTeam ? kTrackedPlayerSameTeam : kTrackedPlayerOtherTeam;
    }
    return trackedCreatureIcon(f.reaction);
}

/// What 0x0057f7f0 asks of a game object.
struct GameObjectFacts {
    /// The menu's mailbox entry finds it: its type, the player not a ghost
    /// and it not hostile (0x0070f850, 0x0071f8b0).
    bool otherTracked = false;
    uint8_t questStatus = 0;
    bool resourceTracked = false;  ///< its lock in PLAYER_TRACK_RESOURCES (0x006dca90)
};
constexpr int gameObjectIcon(const GameObjectFacts& f, bool trackingLowLevel) {
    if (f.otherTracked) return kTrackedResource;
    const int quest = questGiverIcon(f.questStatus, trackingLowLevel);
    if (quest >= 0) return quest;
    return f.resourceTracked ? kTrackedResource : -1;
}

/// Whether an object gets a blip, and grey, by the indoor areas (0x0057f7f0
/// on 0x0077f090 and 0x0077f160): with the player inside a building's
/// interior group, only what is inside the same building; otherwise all,
/// what stands inside some building greyed. `playerArea` and `objectArea`
/// are the building instance, 0 for none.
struct AreaVerdict {
    bool shown = true;
    bool grey = false;
};
constexpr AreaVerdict areaVerdict(uint32_t playerArea, uint32_t objectArea) {
    if (playerArea != 0) return {objectArea == playerArea, false};
    return {true, objectArea != 0};
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
