#pragma once

// How many sounds of a kind the client lets play at once.
//
// Every play 0x004c6a40 starts is of a type: an index into 0x009f2010's
// names - the channel whose volume it is heard at - and into 0x009f2268's
// counts. The play is counted under its type as it starts (0x00b4a340, up
// at 0x004c6a40, down at its end in 0x004c62b0), and one asked for while
// its type already has as many playing as the count allows is refused
// (0x004c6a40 returns 0xe). Types 0 to 5 - SFX, MUSIC, AMBIENCE, CINEMATIC,
// SCRIPTSOUND, SCRIPTMUSIC - are counted to no end; past them come types
// heard at the effects volume with a handful of voices each, so a crowd's
// worth of the same sound does not pile up:
//
//    7  1  the player's error speech (0x00986d00)
//    8  2  a unit's race splash, wading in or out (0x00746720)
//    9  1  a unit's Exertion and ExertionCritical (0x00746b30)
//   10  2  a melee swing's whoosh (0x004d0720)
//   11  2  a unit's Injury, InjuryCritical and InjuryCrushingBlow
//   12  1
//   13  6  a unit's footfall, any but the active player's (0x004cf170)
//   14  4  a weapon's impact (0x004d0540, 0x004d0660) and a creature's
//          custom attack sound (0x00756240)
//   15  1  9, for a creature the server flags as more audible
//   16  2  11, likewise
//   17  4  the active player's footfall (0x004cf170)
//
// and 6, RACIALCINEMATIC, one at a time.

#include <cstdint>
#include <limits>

namespace wowee::audio {

/// The client's play types that are counted, by their index. None is any
/// other play: never refused for the number already playing.
enum class PlayLimit : uint8_t {
    None = 0,
    ErrorSpeech = 7,
    RaceSplash = 8,
    Exertion = 9,
    WeaponSwing = 10,
    Injury = 11,
    UnitFootstep = 13,
    WeaponImpact = 14,
    ExertionAudible = 15,
    InjuryAudible = 16,
    OwnFootstep = 17,
};

namespace play_limit {

/// How many of a type may play at once (0x009f2268).
constexpr uint32_t maxPlaying(PlayLimit type) {
    constexpr uint32_t kAny = std::numeric_limits<int32_t>::max();
    constexpr uint32_t kCount[18] = {kAny, kAny, kAny, kAny, kAny, kAny, 1, 1, 2, 1, 2, 2, 1, 6, 4, 1, 2, 4};
    const auto i = static_cast<uint8_t>(type);
    return i < 18 ? kCount[i] : 0u;
}

/// Whether one more of a type starts with `playing` of it already playing.
constexpr bool admits(PlayLimit type, uint32_t playing) {
    return type == PlayLimit::None || playing < maxPlaying(type);
}

}  // namespace play_limit
}  // namespace wowee::audio
