#pragma once

// A model's events that play sounds, as the client plays them.
//
// The client steps a model's event tracks with its animation (0x00832260):
// each event whose time, in the sequence the model is playing, falls after
// where the last step got to and at or before where this one gets to fires
// once (0x00830fb0), again every lap where the sequence loops, and its
// owner's callback (CM2Model +0x1c4, set by 0x00824060) gets the event's id,
// its data and where it is in the world - its position on its bone.
//
// Of the owners whose callbacks play sounds:
//   a game object (0x0070cac0 -> its type's slot 0x40 -> 0x0070c050):
//     $GO0..$GO5 and $GC0..$GC3 its GameObjectDisplayInfo Sound[0..9]
//     (0x0070bf70); $SND and $DSO the SoundEntries id in the event's data,
//     once, where the event is; $DSL that one looping on the object;
//   a spell effect (0x006f7b00): $SND the data's sound, at the event, or -
//     on the active player, with Sound_ListenerAtCharacter (default 1) - at
//     the listener at 0.65 of its volume.
//   a map doodad, one an ADT or a WMO places (0x007becd0, 0x007bef40,
//     both registering 0x007bd5a0): $DSO the data's sound once, where the
//     event is; $DSL that one looping on the doodad, rising over 3 seconds;
//     $DSE ends the doodad's loop.
// A unit's callback (0x00734a40 -> 0x00732650) plays its CreatureSoundData
// and footsteps instead, and none of these. A dynamic object's (0x00704c30:
// $SND, $SHK) has no model here to own: wowee draws no dynamic object. A
// spell missile's model is given no callback at all (0x00733820 makes it with
// 0x0081f8f0 alone), so its events do nothing.
//
// A game object's $SHK shakes the camera: the data is a
// SpellEffectCameraShakes row, each of whose CameraShakes goes on the camera
// where the event is (0x0070c050 -> 0x00606410).
//
// A doodad's $EAC, $EDC, $EWT and $EMV set the ObjectEffect state of the
// game object the doodad belongs to (0x006f7270, 0x006f61d0); wowee has no
// ObjectEffect, and they do nothing here.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::pipeline { class AssetManager; }

namespace wowee::rendering {

class M2Renderer;

namespace m2_event {

/// Four characters read as a little-endian word, as the client compares them.
constexpr uint32_t fourCC(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

constexpr uint32_t kSound = fourCC("$SND");          // 0x444e5324
constexpr uint32_t kDoodadSoundOnce = fourCC("$DSO"); // 0x4f534424
constexpr uint32_t kDoodadSoundLoop = fourCC("$DSL"); // 0x4c534424
constexpr uint32_t kDoodadSoundEnd = fourCC("$DSE");  // 0x45534424
constexpr uint32_t kCameraShake = fourCC("$SHK");     // 0x4b485324

/// How long a loop takes to fade once its owner goes (0x008799e0 sets a
/// looping sound's fade out to 0.5 as its sound object lets it go).
constexpr float kOwnerGoneFadeSeconds = 0.5f;

/// Whose model an instance is, which decides what its events do.
enum class Owner : uint8_t {
    None,        ///< no callback: its events do nothing
    GameObject,  ///< 0x0070c050; the argument is its display id
    Effect,      ///< 0x006f7b00; the argument is 1 on the active player
    Doodad,      ///< 0x007bd5a0, a map doodad's
};

/// The GameObjectDisplayInfo Sound slot a game object's event plays
/// (0x0070c050 hands 0x0070bf70 the slot in edx): $GO0..$GO5 are 0..5,
/// $GC0..$GC3 6..9. None for any other event.
inline std::optional<uint32_t> gameObjectSoundSlot(uint32_t eventId) {
    const uint32_t prefix = eventId & 0x00FFFFFFu;
    const uint32_t digit = (eventId >> 24) - '0';
    if (prefix == (fourCC("$GO0") & 0x00FFFFFFu) && digit <= 5) return digit;
    if (prefix == (fourCC("$GC0") & 0x00FFFFFFu) && digit <= 3) return 6 + digit;
    return std::nullopt;
}

/// How an event's sound is played.
enum class Play : uint8_t {
    None,
    /// Once, where the event is, owned by nothing (0x004c6a40 with no
    /// sound object).
    Once,
    /// A game object's sound by its kit: a looping kit (SoundEntries Flags
    /// 0x200, 0x004c5bf0) on the object, unless the object already plays one
    /// (0x004c5be0) or the kit already plays within sqrt(6) of the event
    /// (0x004cfe00); any other kit once.
    ObjectKit,
    /// Looping on the object, whatever the kit says, with the same two
    /// tests ($DSL, 0x0070c050 and 0x007bd5a0).
    ObjectLoop,
    /// The owner's loop ends ($DSE, 0x007bd5a0 -> 0x004c6390).
    StopLoop,
    /// A spell effect's: on the effect (0x006f7b00 hands 0x004c6a40 its
    /// sound object, +0x34), so a looping kit goes on until it goes away.
    OnEffect,
};

struct SoundPlan {
    Play play = Play::None;
    /// The SoundEntries id, or the display's Sound slot for a game object's
    /// $GOn / $GCn (slotted).
    uint32_t sound = 0;
    bool slotted = false;
    /// A loop's fade in and fade out, seconds (the play's +0x10 and +0x14,
    /// 0x004c6a40 -> 0x008785f0, 0x00878610): a game object's $DSL 1 and 1,
    /// a doodad's 3 in; none for any other.
    float fadeIn = 0.0f;
    float fadeOut = 0.0f;
};

/// What an event of an owner's model plays.
inline SoundPlan planSound(Owner owner, uint32_t eventId, uint32_t data) {
    switch (owner) {
    case Owner::GameObject:
        if (const auto slot = gameObjectSoundSlot(eventId))
            return {.play = Play::ObjectKit, .sound = *slot, .slotted = true};
        if (eventId == kSound || eventId == kDoodadSoundOnce) return {.play = Play::Once, .sound = data};
        if (eventId == kDoodadSoundLoop)
            return {.play = Play::ObjectLoop, .sound = data, .fadeIn = 1.0f, .fadeOut = 1.0f};
        return {};
    case Owner::Doodad:
        if (eventId == kDoodadSoundOnce) return {.play = Play::Once, .sound = data};
        if (eventId == kDoodadSoundLoop) return {.play = Play::ObjectLoop, .sound = data, .fadeIn = 3.0f};
        if (eventId == kDoodadSoundEnd) return {.play = Play::StopLoop};
        return {};
    case Owner::Effect:
        if (eventId == kSound) return {.play = Play::OnEffect, .sound = data};
        return {};
    case Owner::None:
        break;
    }
    return {};
}

/// Whether an owner's event shakes the camera, by the SpellEffectCameraShakes
/// row in its data: a game object's $SHK (0x0070c050).
inline bool shakesCamera(Owner owner, uint32_t eventId) {
    return owner == Owner::GameObject && eventId == kCameraShake;
}

/// Each of `times` (ms in the sequence, sorted) the animation passed in
/// going from `fromMs` to `toMs`: after the one and at or before the other
/// (0x00830fb0). With a `lapMs` the sequence loops, and each time comes
/// round again every lap; without, it is passed once at most. A sequence
/// just started is looked at from below zero, so that its first frame's
/// events fire.
template <typename Fire>
void forEachCrossed(const std::vector<uint32_t>& times, float fromMs, float toMs, float lapMs, Fire&& fire) {
    if (times.empty() || !(toMs > fromMs)) return;
    if (!(lapMs > 0.0f)) {
        for (uint32_t t : times) {
            const float at = static_cast<float>(t);
            if (at > fromMs && at <= toMs) fire(t);
        }
        return;
    }
    // From the lap `fromMs` is in, a lap at a time, in order.
    for (float lap = std::floor(fromMs / lapMs) * lapMs; lap <= toMs; lap += lapMs) {
        for (uint32_t t : times) {
            const float at = lap + static_cast<float>(t);
            if (at > toMs) break;
            if (at > fromMs) fire(t);
        }
    }
}

}  // namespace m2_event

/// Plays the sounds of the model events M2Renderer::update found.
class M2EventSounds {
public:
    /// Shakes the camera by a SpellEffectCameraShakes row, where an event is.
    using ShakeCamera = std::function<void(uint32_t shakesId, const glm::vec3& at)>;
    /// Play this frame's events, and follow (and end) the loops they start.
    void update(M2Renderer& m2, pipeline::AssetManager* assets, const ShakeCamera& shakeCamera = {});
    /// Stop every loop, as the world goes.
    void clear();

private:
    struct Kit {
        std::vector<std::vector<uint8_t>> files;  ///< each file's data
        std::vector<uint32_t> frequency;          ///< each file's weight
        float volume = 1.0f;                      ///< VolumeFloat
        float minDistance = 0.0f;                 ///< MinDistance
        float cutoff = 0.0f;                      ///< DistanceCutoff
        bool loops = false;                       ///< Flags 0x200
    };
    const Kit* kit(uint32_t soundId, pipeline::AssetManager* assets);
    const std::vector<uint8_t>* pickFile(const Kit& k);
    uint32_t displaySound(uint32_t displayId, uint32_t slot, pipeline::AssetManager* assets);

    struct Loop {
        uint32_t handle = 0;
        uint32_t soundId = 0;
        glm::vec3 offset{0.0f};    ///< from the instance's origin, where it started
        glm::vec3 position{0.0f};  ///< where it is now
        float fadeOut = 0.0f;      ///< how long a $DSE takes to end it
    };
    std::unordered_map<uint32_t, Kit> kits_;
    std::unordered_map<uint32_t, std::array<uint32_t, 10>> displaySounds_;
    bool displaySoundsLoaded_ = false;
    /// The loop an instance plays, by instance id: one each, as the client's
    /// object has one sound object (0x004c5be0).
    std::unordered_map<uint32_t, Loop> loops_;
};

}  // namespace wowee::rendering
