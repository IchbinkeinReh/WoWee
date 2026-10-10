#pragma once

#include <cstdint>

namespace wowee::audio::screen_effect_audio {

/// The client's sound day: from 5:30 to 21:00 (0x004c9850). Its index into
/// a slot's day and night sound (0x00ac3878) is 0 in it, 1 out of it.
constexpr bool isSoundDaytime(float hours) {
    const float minutes = hours * 60.0f;
    return minutes >= 5.0f * 60.0f + 30.0f && minutes < 21.0f * 60.0f;
}

/// The sound a SoundAmbience row (+4 day, +8 night) or a ZoneMusic row
/// (+0x18 day, +0x1c night) gives for the time.
constexpr uint32_t dayOrNight(uint32_t day, uint32_t night, bool isDay) { return isDay ? day : night; }

/// ScreenEffect.dbc's +0x20 and +0x24, a SoundAmbience and a ZoneMusic row,
/// go to the top of the client's eleven sound slots (0x004f7020 ->
/// 0x004c8fa0, slot 10): the highest slot with a sound plays (0x004c8d80
/// walks from 10 down), so while the effect is up its ambience and music
/// take the place of the zone's. A row of 0 or none empties the slot. Only
/// slots 0 to 2 wait out the zone music's silence between tracks; slot 10's
/// plays at once and again when its track ends (0x004c87d0, 0x004c8830).
/// The DBC columns read:
inline constexpr uint32_t kSoundAmbienceDayCol = 1, kSoundAmbienceNightCol = 2;
inline constexpr uint32_t kZoneMusicDayCol = 6, kZoneMusicNightCol = 7;

}  // namespace wowee::audio::screen_effect_audio
