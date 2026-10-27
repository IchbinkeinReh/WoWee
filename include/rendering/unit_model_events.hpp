#pragma once

// What a unit's model events play, as the client's unit callback plays them.
//
// A unit's model, and the mount model it rides (0x0073d5d0), call the unit
// back with each event its animation passes (0x00824060 -> 0x00734a40 ->
// 0x00732650). Of what that callback does, the sounds of the unit's
// CreatureSoundData:
//
//   $FD1..$FD5   the row's Fidget sound 1..5, once, two yards over the unit
//                (0x007471a0 -> 0x007462e0);
//   $WNG, $WGG   its WingFlap and WingGlide sound, and $FDX its Stand
//                sound for any unit but the active player - each by the
//                slot's chance (0x00747310, 0x00adb5d8), Stand at most once
//                in ten seconds of all units;
//   $BRT, $SCD,  its Birth, SpellCastDirected, Submerge and Submerged
//   $SMG, $SMD   sound, once, where the event is.
//
// The row is the unit's display's: CreatureDisplayInfo SoundID, else its
// CreatureModelData's (+0x974, set by 0x0072d940). The fidgets and the
// slots read it through 0x0071a3f0, which takes the row's
// CreatureSoundDataIDPet in its place while the unit has a
// UNIT_FIELD_PETNUMBER.
//
// And $FSD, a footfall (0x00747240 -> 0x004cf170): the row's footstep id
// (CreatureSoundData +0x24) and the ground the unit stands on pick a
// FootstepTerrainLookup sound - its splash where the foot is in water - see
// FootstepTerrainSounds.

#include "audio/play_limit.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wowee::rendering::unit_event {

/// Four characters read as a little-endian word, as the client compares them.
constexpr uint32_t fourCC(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

enum class Kind : uint8_t {
    None,
    /// CreatureSoundData Fidget[index], over the unit (0x007462e0).
    Fidget,
    /// A creature sound slot, by its chance (0x00747310).
    Slot,
    /// A CreatureSoundData column of the display's own row, at the event.
    AtEvent,
    /// A footfall, by the ground under the unit (0x00747240).
    Footstep,
};

struct Action {
    Kind kind = Kind::None;
    /// Fidget: 0..4. Slot: the slot. AtEvent: the column.
    uint32_t index = 0;
};

/// The creature sound slots 0x00747310 is asked to play by an event.
constexpr uint32_t kSlotStand = 5;      ///< $FDX
constexpr uint32_t kSlotWingFlap = 7;   ///< $WNG
constexpr uint32_t kSlotWingGlide = 10; ///< $WGG

/// The slots a jump's own sound is, asked outright (0x0073ed10, 0x0073d2b0):
/// as the unit leaves the ground, and as it comes down again - a jump, a
/// take-off into flight and a landing out of it. Only 30 of the 1306
/// CreatureSoundData rows name either: 25 of those the universal flying
/// mount's (SoundEntries 10905 take-off, 10904 landing), and no player's -
/// a character on foot and on a flying mount alike is silent here.
constexpr uint32_t kSlotJumpStart = 11;
constexpr uint32_t kSlotJumpEnd = 12;

/// CreatureSoundData columns (3.3.5a, 38 of them).
constexpr uint32_t kColumnFootstep = 9;  ///< +0x24, FootstepTerrainLookup's CreatureFootstepID
constexpr uint32_t kColumnFidget0 = 14;
constexpr uint32_t kColumnBirth = 33;
constexpr uint32_t kColumnSpellCastDirected = 34;
constexpr uint32_t kColumnSubmerge = 35;
constexpr uint32_t kColumnSubmerged = 36;
constexpr uint32_t kColumnPetRow = 37;  ///< CreatureSoundDataIDPet
constexpr uint32_t kColumnCount = 38;

/// What an event of a unit's model does with its sounds.
constexpr Action classify(uint32_t eventId) {
    for (uint32_t i = 0; i < 5; ++i) {
        const char name[5] = {'$', 'F', 'D', static_cast<char>('1' + i), 0};
        if (eventId == fourCC(name)) return {Kind::Fidget, i};
    }
    if (eventId == fourCC("$WNG")) return {Kind::Slot, kSlotWingFlap};
    if (eventId == fourCC("$WGG")) return {Kind::Slot, kSlotWingGlide};
    if (eventId == fourCC("$FDX")) return {Kind::Slot, kSlotStand};
    if (eventId == fourCC("$BRT")) return {Kind::AtEvent, kColumnBirth};
    if (eventId == fourCC("$SCD")) return {Kind::AtEvent, kColumnSpellCastDirected};
    if (eventId == fourCC("$SMG")) return {Kind::AtEvent, kColumnSubmerge};
    if (eventId == fourCC("$SMD")) return {Kind::AtEvent, kColumnSubmerged};
    if (eventId == fourCC("$FSD")) return {Kind::Footstep, kColumnFootstep};
    return {};
}

/// The CreatureSoundData column a slot plays (0x007461e0), 0 for none.
constexpr uint32_t slotColumn(uint32_t slot) {
    switch (slot) {
    case 0: return 1;    // Exertion
    case 1: return 2;    // ExertionCritical
    case 2:
    case 13: return 3;   // Injury
    case 3: return 4;    // InjuryCritical
    case 4: return 7;    // Stun
    case 5: return 8;    // Stand
    case 7: return 11;   // WingFlap
    case 8: return 13;   // Alert
    case 9: return 5;    // InjuryCrushingBlow
    case 10: return 12;  // WingGlide
    case kSlotJumpStart: return 26;  // JumpStart
    case kSlotJumpEnd: return 27;    // JumpEnd
    default: return 0;
    }
}

/// The slots' chances, out of 100 (0x00adb5d8).
constexpr uint32_t slotChance(uint32_t slot) {
    constexpr uint32_t kChance[13] = {70, 100, 60, 100, 100, 40, 100, 100, 100, 100, 100, 100, 100};
    return slot < 13 ? kChance[slot] : 0;
}

/// Whether a slot plays for a 32-bit random draw: the draw times 101, its
/// high word, no more than the chance (0x00747310) - so 40 plays 41 times in
/// 101, and 100 always.
constexpr bool slotPlays(uint32_t slot, uint32_t random32) {
    const auto roll = static_cast<uint32_t>((static_cast<uint64_t>(random32) * 101u) >> 32);
    return roll <= slotChance(slot);
}

/// The slots a melee hit asks for: the attacker's exertion as the hit
/// arrives (0x00755130), the target's injury as it lands (0x00755e40).
constexpr uint32_t kSlotExertion = 0, kSlotExertionCritical = 1, kSlotInjury = 2, kSlotInjuryCritical = 3,
                   kSlotInjuryCrushingBlow = 9;

/// The play type a slot's sound is counted as (0x00746b30): the exertions
/// 9, the injuries 11 - 15 and 16 for a creature whose cache's type flags
/// have 0x20, which is heard over more of them; the rest uncounted.
constexpr uint32_t kCreatureTypeFlagMoreAudible = 0x20;
constexpr audio::PlayLimit slotPlayType(uint32_t slot, bool moreAudible) {
    switch (slot) {
    case kSlotExertion:
    case kSlotExertionCritical:
        return moreAudible ? audio::PlayLimit::ExertionAudible : audio::PlayLimit::Exertion;
    case kSlotInjury:
    case kSlotInjuryCritical:
    case kSlotInjuryCrushingBlow:
        return moreAudible ? audio::PlayLimit::InjuryAudible : audio::PlayLimit::Injury;
    default:
        return audio::PlayLimit::None;
    }
}

/// $FDX's Stand sound: at most once in this long, of every unit (0x00ca12d4).
constexpr uint32_t kStandSoundIntervalMs = 10000;
constexpr bool standSoundDue(uint32_t nowMs, uint32_t lastMs) {
    return static_cast<int32_t>(nowMs - lastMs - kStandSoundIntervalMs) >= 0;
}

/// A slot's sound for the active player, with the listener at the
/// character (Sound_ListenerAtCharacter, default 1): where the listener is,
/// at this much of its volume (0x00747310).
constexpr float kListenerAtCharacterVolume = 0.65f;

/// The fidgets: two yards over the unit (0x007462e0).
constexpr float kFidgetHeight = 2.0f;

/// The footfall sounds by creature footstep id and ground (0x004cf990
/// builds them from FootstepTerrainLookup, 0x004cf100 reads them).
class FootstepTerrainSounds {
public:
    /// A TerrainType row: its id and its SoundID (TerrainTypeSounds), +0x10.
    void addTerrainType(int32_t terrainType, uint32_t soundIndex) { terrainSounds_[terrainType] = soundIndex; }
    /// A FootstepTerrainLookup row: CreatureFootstepID, TerrainSoundID,
    /// SoundID, SoundIDSplash. One whose TerrainSoundID is below zero is
    /// left out (0x004cf990).
    void addLookup(uint32_t creatureFootstepId, int32_t terrainSoundIndex, uint32_t sound, uint32_t splash) {
        if (terrainSoundIndex < 0) return;
        bySound_[key(creatureFootstepId, static_cast<uint32_t>(terrainSoundIndex))] = {sound, splash};
    }
    [[nodiscard]] bool empty() const { return bySound_.empty(); }

    /// 0x004cf100: the terrain type's TerrainType row, its sound index, and
    /// the footstep id's sound for it - the splash where `inWater`. 0 for
    /// none.
    [[nodiscard]] uint32_t find(uint32_t creatureFootstepId, int32_t terrainType, bool inWater) const {
        const auto t = terrainSounds_.find(terrainType);
        if (t == terrainSounds_.end()) return 0;
        const auto it = bySound_.find(key(creatureFootstepId, t->second));
        if (it == bySound_.end()) return 0;
        return inWater ? it->second.splash : it->second.sound;
    }
    /// 0x004cf170: the ground's, else - for none, or no terrain type at all
    /// (-1) - terrain type 0's (Dirt).
    [[nodiscard]] uint32_t sound(uint32_t creatureFootstepId, int32_t terrainType, bool inWater) const {
        const uint32_t s = find(creatureFootstepId, terrainType, inWater);
        return s != 0 ? s : find(creatureFootstepId, 0, inWater);
    }

private:
    struct Sounds {
        uint32_t sound = 0;
        uint32_t splash = 0;
    };
    static uint64_t key(uint32_t creature, uint32_t terrainSound) {
        return (static_cast<uint64_t>(creature) << 32) | terrainSound;
    }
    std::unordered_map<int32_t, uint32_t> terrainSounds_;
    std::unordered_map<uint64_t, Sounds> bySound_;
};

/// The terrain type a unit stands on, as its collision has it (0x007c2a70):
/// a building's floor's material's ground type where the probe down from
/// the unit meets the building first (0x007b39b0), else the terrain's
/// (0x007a0530: GroundEffectTexture TerrainType of the quad's layer, by x
/// and y alone - `terrainFloor` is only the terrain met on the way down, if
/// any), else none (-1). A doodad's model is no floor to it: the probe meets
/// buildings and the terrain only (0x007c28f0).
inline int32_t groundTerrainType(std::optional<float> buildingFloor, int32_t buildingGroundType,
                                 std::optional<float> terrainFloor, std::optional<int32_t> terrainGroundType) {
    if (buildingFloor && (!terrainFloor || *buildingFloor >= *terrainFloor)) return buildingGroundType;
    if (terrainGroundType) return *terrainGroundType;
    return -1;
}

/// Whether a footfall splashes (0x0071a030): liquid where the unit is, the
/// foot below its surface (by less than 0.01 above it, too), and the unit
/// not swimming (MOVEMENTFLAG_SWIMMING, 0x00200000).
constexpr uint32_t kMoveFlagSwimming = 0x00200000u;
inline bool footInWater(std::optional<float> liquidLevel, float footZ, uint32_t moveFlags) {
    return liquidLevel && footZ < *liquidLevel + 0.01f && (moveFlags & kMoveFlagSwimming) == 0;
}

/// UNIT_FIELD_BYTES_1's third byte's hover flag: a hovering unit's
/// footfalls are silent (0x00747240).
constexpr uint32_t kBytes1HoverFlag = 0x02u;
constexpr bool hovers(uint32_t bytes1) { return ((bytes1 >> 16) & kBytes1HoverFlag) != 0; }

/// MOVEMENTFLAG_HOVER, and PLAYER_FLAGS_GHOST.
constexpr uint32_t kMoveFlagHover = 0x40000000u;
constexpr uint32_t kPlayerFlagGhost = 0x10u;

/// Whether a unit's footfall is silent before its sounds are looked at
/// (0x00747240's four tests): hovering by UNIT_FIELD_BYTES_1 (its fields'
/// +0x112 & 2) or by its movement (+0x7cc & MOVEMENTFLAG_HOVER); using a
/// vehicle - seated, or on its way into or out of a seat (its passenger
/// component's state, +0xf60 +0x14, UnitUsingVehicle's test); or a player's
/// ghost (PLAYER_FLAGS & 0x10 through +0x1008, UnitIsGhost's test).
constexpr bool footfallSilent(uint32_t bytes1, uint32_t moveFlags, uint8_t seatState, bool isPlayer,
                              uint32_t playerFlags) {
    return hovers(bytes1) || (moveFlags & kMoveFlagHover) != 0 || seatState != 0 ||
           (isPlayer && (playerFlags & kPlayerFlagGhost) != 0);
}

// An emote's voice line, which is not in the unit's CreatureSoundData row at
// all: EmotesTextSound says which race's and which sex's clip answers an
// EmotesText id. The client reads the table as five columns and a twenty-byte
// row (0x0063e4f0 insists on both) and asks for the line through the same play
// a unit's slots go through - 0x00746d60, whose emote-flagged branch holds both
// emote CVars.
constexpr uint32_t kEmoteSoundColumnEmotesText = 1;
constexpr uint32_t kEmoteSoundColumnRace = 2;
constexpr uint32_t kEmoteSoundColumnSex = 3;
constexpr uint32_t kEmoteSoundColumnSound = 4;
constexpr uint32_t kEmoteSoundColumnCount = 5;

/// One of its rows, held once the table has been read.
struct EmoteSoundRow {
    uint32_t emotesTextId = 0;
    uint32_t race = 0;
    uint32_t sex = 0;
    uint32_t soundId = 0;
};

/// Which clip an emote asks for: the row naming the emote text, the speaker's
/// race and its sex - 3.3.5a's table has one row per those three, two to
/// twenty rows per emote. Nothing asks where no row answers, which is how a
/// speaker whose race and sex are not known yet stays quiet, and a creature
/// with it: every row names a playable race and one of the two sexes.
inline uint32_t emoteSoundId(const std::vector<EmoteSoundRow>& rows, uint32_t emotesTextId,
                             uint32_t race, uint32_t sex) {
    for (const auto& row : rows)
        if (row.emotesTextId == emotesTextId && row.race == race && row.sex == sex) return row.soundId;
    return 0;
}

}  // namespace wowee::rendering::unit_event
