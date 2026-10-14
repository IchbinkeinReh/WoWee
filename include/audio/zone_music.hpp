#pragma once

// The world's music, as the client picks and plays it.
//
// The music shares the ambience's eleven sound slots (0xb4aab0, nine words
// each; audio/zone_ambience.hpp): each has a day and a night music sound
// (+0, +4 - SoundEntries ids, -1 for none) and the silence after it, its
// least (+8, +0xc) and most (+0x10, +0x14) in milliseconds. Six are filled:
//   0  the place's ZoneMusic (0x0078e9a0 -> 0x004c8d80): the linked
//      building group's WMOAreaTable row, the building's own (group -1),
//      then - outside only - the area's AreaTable row and its zone's, the
//      chain the ambience takes;
//   1  the world state's: WorldStateZoneSounds.dbc's ZoneMusic (0x004cbe70);
//   3  SMSG_PLAY_MUSIC's sound (0x004c92b0), emptied when it ends
//      (0x004c90c0);
//   5  the place's ZoneIntroMusicTable row, by the same chain (0x004c8cf0);
//   6  the world state's ZoneIntroMusicTable row;
//  10  the screen effect's ZoneMusic (0x004c8fa0).
// Slots 2 and 7 take WorldChunkSounds.dbc's (0x004c68e0), which ships empty.
// A ZoneMusic row gives Sounds (+0x18 day, +0x1c night) and the silences
// (+8..+0x14); an intro row its SoundID (+8) for both and its
// MinDelayMinutes (+0x10) in the silence's least.
//
// Every frame (0x004c9910) the music channel pair (0xb4ac90) is given the
// highest slot's music (0x004c88b0) unless it plays it already: the new
// track rises over four seconds - a hundredth of one out of quiet - and the
// one before falls over four (0x004c7140). A track plays once unless its
// SoundEntries row loops (Flags 0x200). Nothing to pick lets what plays
// fall out. Nothing starts while the script's PlayMusic plays (0xb4aaa4).
//
// What comes after a track (0x004c5d20 hands each one a note for its end):
//   slots 0..2: the silence (0x004c8ab0). If the slots still pick the same
//      sound, nothing plays for a time between the least and the most;
//      otherwise the next starts at once. Slots 0..2 pick the silence while
//      it lasts (0x004c87d0) - none of them plays, the slots above can -
//      unless Sound_ZoneMusicNoDelay is set.
//   slot 3: the slot is emptied.
//   slots 5..7: the intro may not play again for its MinDelayMinutes
//      (0x004c9080). An intro plays while its own isn't due (0x004c8830)
//      and not straight after another intro; one playing goes on to its end
//      whatever the slots say.
// A new ZoneMusic row in a slot ends the silence when the slots pick other
// than what plays - but not the silence slots 0..2 are waiting out
// (0x004c8d80): music entering a zone during the silence waits for it.

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wowee::audio::zone_music {

inline constexpr int kSlotCount = 11;
inline constexpr int kSlotPlace = 0, kSlotWorldState = 1, kSlotServer = 3, kSlotPlaceIntro = 5,
                     kSlotWorldStateIntro = 6, kSlotScreenEffect = 10;
/// The slot of a channel record holding nothing (0x004c6450, 0x004c7140).
inline constexpr int kNoSlot = 13;
/// What 0x004c88b0 answers while slots 0..2 wait: sound -0x457 in slot 12.
inline constexpr int32_t kSilence = -1111;
inline constexpr int kSilenceSlot = 12;
/// The rise and the fall of a change of track (0x004c9910: 4.0), and the
/// rise of one out of quiet (0.01).
inline constexpr float kFadeSeconds = 4.0f;
inline constexpr float kFadeFromQuietSeconds = 0.01f;
/// A SoundEntries row that loops (0x004c6a40 reads Flags +0x3c & 0x200).
inline constexpr uint32_t kSoundLoops = 0x200;

/// A slot's music: day and night sound, -1 for none, and the silence after.
struct SlotMusic {
    std::array<int32_t, 2> sound{-1, -1};
    std::array<uint32_t, 2> silenceMin{0, 0};
    std::array<uint32_t, 2> silenceMax{0, 0};
    bool operator==(const SlotMusic&) const = default;
};
using Slots = std::array<SlotMusic, kSlotCount>;

/// The client's day for the slots (0x00ac3878): 0 by day, 1 by night.
constexpr int timeIndex(bool isDay) { return isDay ? 0 : 1; }

/// A ZoneMusic.dbc row: SilenceIntervalMin, SilenceIntervalMax, Sounds -
/// each day then night.
struct ZoneMusicRow {
    std::array<uint32_t, 2> silenceMin{0, 0};
    std::array<uint32_t, 2> silenceMax{0, 0};
    std::array<int32_t, 2> sounds{-1, -1};
};
/// A ZoneIntroMusicTable.dbc row: SoundID and MinDelayMinutes. Its Priority
/// is read by nothing.
struct IntroRow {
    int32_t sound = -1;
    uint32_t minDelayMinutes = 0;
};

/// 0x004c8d80: a slot's music from a ZoneMusic row, emptied without one.
constexpr void fillZoneMusic(SlotMusic& slot, const ZoneMusicRow* row) {
    if (!row) {
        slot = SlotMusic{};
        return;
    }
    slot.sound = row->sounds;
    slot.silenceMin = row->silenceMin;
    slot.silenceMax = row->silenceMax;
}
/// 0x004c8cf0: an intro's sound by day and night, its delay where a
/// silence's least goes. A row not there empties the slot; one there leaves
/// the most as it was.
constexpr void fillIntro(SlotMusic& slot, const IntroRow* row) {
    if (!row) {
        slot = SlotMusic{};
        return;
    }
    slot.sound = {row->sound, row->sound};
    slot.silenceMin = {row->minDelayMinutes, row->minDelayMinutes};
}
/// 0x004c92b0: SMSG_PLAY_MUSIC's sound by day and night, no silence.
constexpr void fillServer(SlotMusic& slot, int32_t sound) {
    slot = SlotMusic{};
    slot.sound = {sound, sound};
}

/// The music channel's current record (0xb4ac90): whether it plays, its
/// sound and the slot it was started for.
struct Channel {
    bool playing = false;
    int32_t sound = 0;
    int slot = kNoSlot;
    /// 0x004c6410: the sound while it plays, 0 otherwise.
    constexpr int32_t playingSound() const { return playing ? sound : 0; }
};

/// What the slots pick: the slot (kNoSlot for nothing, kSilenceSlot for the
/// silence), the sound, and the silence after it.
struct Pick {
    int slot = kNoSlot;
    int32_t sound = 0;
    uint32_t silenceMin = 0;
    uint32_t silenceMax = 0;
    bool operator==(const Pick&) const = default;
};

/// What the pick depends on besides the slots.
struct Context {
    /// Sound_EnableAllSound and Sound_EnableMusic, no movie playing (0xbd0800).
    bool enabled = true;
    /// Sound_ZoneMusicNoDelay.
    bool noDelay = false;
    /// 0 by day, 1 by night (0x00ac3878).
    int time = 0;
    /// Milliseconds (0x0086ae20).
    uint64_t now = 0;
    /// The end of the silence (0xb4aaa0).
    uint64_t silenceUntil = 0;
    /// When each intro may play again, by sound (0xb4acd8); not there is now.
    const std::unordered_map<int32_t, uint64_t>* introDue = nullptr;
};

constexpr bool isIntroSlot(int slot) { return slot == 5 || slot == 6 || slot == 7; }

/// 0x004c87d0: slots 0..2 wait out the silence, unless there is to be none.
constexpr bool silenceOver(int slot, const Context& c) {
    if (slot < 0 || slot > 2 || c.noDelay) return true;
    return c.silenceUntil <= c.now;
}

/// 0x004c8830: a slot's sound may play - what plays already may; an intro
/// not straight after another and once it is due.
inline bool mayPlay(int32_t sound, int slot, const Channel& ch, const Context& c) {
    if (sound == ch.playingSound() || !isIntroSlot(slot)) return true;
    if (isIntroSlot(ch.slot)) return false;
    if (!c.introDue) return true;
    const auto it = c.introDue->find(sound);
    return it == c.introDue->end() || it->second <= c.now;
}

/// 0x004c88b0: from slot 10 down, the first that has music for the time and
/// may play it - or, at an intro's slot, an intro already playing; slots 0..2
/// in their silence give the silence. Disabled music picks nothing.
inline Pick pick(const Slots& slots, const Channel& ch, const Context& c) {
    if (!c.enabled) return Pick{14, 0, 0, 0};
    const auto t = static_cast<size_t>(c.time);
    for (int slot = kSlotCount - 1; slot >= 0; --slot) {
        if (isIntroSlot(slot) && ch.playing && isIntroSlot(ch.slot)) return Pick{ch.slot, ch.playingSound(), 0, 0};
        const SlotMusic& s = slots[static_cast<size_t>(slot)];
        if (s.sound[t] < 0) continue;
        if (!silenceOver(slot, c)) return Pick{kSilenceSlot, kSilence, 0, 0};
        if (mayPlay(s.sound[t], slot, ch, c)) return Pick{slot, s.sound[t], s.silenceMin[t], s.silenceMax[t]};
    }
    return Pick{};
}

/// The script's music hold (0xb4aaa4): PlayMusic's file (0x004c9110(file,
/// 5)) sets it once the file plays - not with Sound_EnableMusic off - and
/// only StopMusic (0x004c8680), the unload (0x004c93b0) and Sound_EnableMusic
/// off clear it. The file looping on, nothing else ends it.
struct ScriptMusicHold {
    bool held = false;
    /// PlayMusic: whether the music is enabled and the file then plays.
    constexpr void played(bool musicEnabled, bool started) {
        if (musicEnabled && started) held = true;
    }
    constexpr void stopped() { held = false; }
};

/// 0x004c9910: whether the channel is given what is picked - not while the
/// script's music plays, nor the silence, nor what plays already.
constexpr bool shouldStart(const Pick& p, const Channel& ch, bool scriptMusicPlaying) {
    if (scriptMusicPlaying || p.sound == kSilence) return false;
    return !ch.playing || ch.playingSound() != p.sound;
}

/// The rise of a track the channel is given (0x004c9910).
constexpr float fadeInSeconds(const Channel& ch) { return ch.playing ? kFadeSeconds : kFadeFromQuietSeconds; }

/// What a track's end does, by the slot it was started for (0x004c9910).
enum class OnEnd { None, Silence, EmptyServerSlot, IntroDelay };
constexpr OnEnd onEnd(int slot) {
    if (slot >= 0 && slot <= 2) return OnEnd::Silence;
    if (slot == kSlotServer) return OnEnd::EmptyServerSlot;
    if (isIntroSlot(slot)) return OnEnd::IntroDelay;
    return OnEnd::None;
}

/// 0x004c8ab0: the end of the silence after a track of slots 0..2 - if the
/// slots still pick its sound, between its least and its most from now (the
/// most where the least is not under it; `random` a 32-bit roll scaled
/// across the gap), and none otherwise.
constexpr uint64_t silenceAfter(int32_t endedSound, uint32_t min, uint32_t max, int32_t pickedNow, uint64_t now,
                                uint32_t random) {
    if (pickedNow != endedSound) return 0;
    if (min >= max) return now + max;
    return now + min + ((static_cast<uint64_t>(random) * (max - min)) >> 32);
}

/// 0x004c9080: when an intro may play again, from its end.
constexpr uint64_t introDueAfter(uint32_t minDelayMinutes, uint64_t now) {
    return now + static_cast<uint64_t>(minDelayMinutes) * 60000u;
}

/// 0x004c8d80: whether a slot given ZoneMusic row `id` (`found` whether the
/// table has it) ends the silence: a row not there does when its id is
/// above 0; one there does when the slots then pick other than what plays,
/// unless what they pick is the silence of slots 0..2.
constexpr bool zoneMusicEndsSilence(int slot, int64_t id, bool found, const Pick& pickedNow, const Channel& ch) {
    if (!found) return id > 0;
    return pickedNow.sound != ch.playingSound() && (slot > 2 || pickedNow.sound != kSilence);
}

/// 0x004cbac0 as the music asks it: one of a kit's files by its Freq
/// weights, never the one played last while another has weight. `random`
/// is a 32-bit roll. None when no file has weight.
inline std::optional<size_t> pickFile(const std::vector<uint32_t>& freq, std::optional<size_t> last,
                                      uint32_t random) {
    if (freq.empty()) return std::nullopt;
    if (freq.size() == 1) return size_t{0};
    const auto total = [&](std::optional<size_t> skip) {
        uint64_t sum = 0;
        for (size_t i = 0; i < freq.size(); ++i)
            if (!skip || i != *skip) sum += freq[i];
        return sum;
    };
    if (total(last) == 0) last.reset();
    const uint64_t sum = total(last);
    if (sum == 0) return std::nullopt;
    uint64_t roll = (static_cast<uint64_t>(random) * sum) >> 32;
    for (size_t i = 0; i < freq.size(); ++i) {
        if (last && i == *last) continue;
        if (roll < freq[i]) return i;
        roll -= freq[i];
    }
    return std::nullopt;
}

}  // namespace wowee::audio::zone_music
