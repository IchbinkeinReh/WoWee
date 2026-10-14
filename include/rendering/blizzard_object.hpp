#pragma once

/// A kit's CharProc 9 on a dynamic object: models falling over its area, as
/// Blizzard and Rain of Fire show (SpellVisuals.cpp, "BlizzardObject").
///
/// Made (0x00704f60, with the kit's sound - so not while the object is held):
/// the first of the kit's four CharProcs that is 9 gives 0x007fbd70 the
/// object's position and DYNAMICOBJECT_RADIUS, its CharParamZero rounded -
/// the low byte a row of SpellVisualKitAreaModel.dbc, counted from 0 - and
/// its CharParamOne, how many fall a second (0x007fb5b0). That rate is scaled
/// by spellEffectLevel (default 9): level / 9 * 0.9 + 0.1.
///
/// Each frame (0x007fc220): the rate times the frame's time, at most 1/15 s,
/// adds up; each whole one past 1 is a model at a random point within the
/// radius - a random fraction of it in a random direction - put on what is
/// below it (landing): from 16.67 above the object's height (or 0.95 of the
/// way to a ceiling met on the slanted way up from 1/6 above the object),
/// the first surface within 33.33 down, else the object's own place. It
/// waits 0 to 510 ms, then plays its Stand once (0x007fb2d0) and goes as
/// that ends (0x007fc200).
/// Every fourth event any of these models fires, if it is a $SND, plays the
/// data's sound once where it is (0x007f9d80).
///
/// On a transport the object hands the effect the transport's matrix each
/// frame (0x007132e0 -> 0x00704910 -> 0x007fa1b0): its place and its drops'
/// are then in the transport's frame (drop, drawnAt). Once the object has
/// gone the last matrix stays.
///
/// The object gone (0x00704940, 0x007054e0 -> 0x007f9ee0) sets the rate to
/// 0: no more fall, and the effect goes once the last has played.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

namespace wowee::rendering::blizzard_object {

inline constexpr uint32_t kCharProc = 9;
/// 0x007fbd70's model where the table has no row at all.
inline constexpr const char* kDefaultModel = "Spells\\Blizzard_Impact_Base.mdx";
/// 0x007fc220: the longest step a frame adds (1/15 s).
inline constexpr float kMaxStepSeconds = 0.06666667f;
/// 0x007fc220: how far above the object a model is looked for from, where
/// the way up starts, how much of the way to a ceiling it keeps, and how far
/// down the floor is sought.
inline constexpr float kRise = 16.666666f;
inline constexpr float kCeilingFrom = 0.16666667f;
inline constexpr float kCeilingKeep = 0.95f;
inline constexpr float kDrop = 33.333332f;

/// 0x00704f60 -> 0x007fbd70: the SpellVisualKitAreaModel row (from 0) the
/// kit's CharParamZero names - rounded, its low byte - the first row where
/// that is past the end, none where the table is empty (kDefaultModel).
inline std::optional<size_t> areaModelRow(float paramZero, size_t rowCount) {
    const uint32_t index = static_cast<uint32_t>(std::lrint(paramZero)) & 0xFFu;
    if (index < rowCount) return index;
    if (rowCount >= 1) return 0;
    return std::nullopt;
}

/// 0x007fb5b0: how many fall a second - the kit's CharParamOne scaled by
/// spellEffectLevel.
inline float rate(float paramOne, int spellEffectLevel) {
    return (static_cast<float>(spellEffectLevel) * 0.11111111f * 0.9f + 0.1f) * paramOne;
}

/// 0x007fc220: the count carried over this frame - the rate times the
/// frame's time (at most kMaxStepSeconds) added - and how many fall: one for
/// each whole one past 1, taken off.
inline uint32_t advance(float& carried, float frameSeconds, float perSecond) {
    carried += std::min(frameSeconds, kMaxStepSeconds) * perSecond;
    uint32_t count = 0;
    while (carried > 1.0f) {
        carried -= 1.0f;
        ++count;
    }
    return count;
}

/// 0x007fc220: where one falls, in the plane - `fraction` of the radius
/// (0x00464580, [0, 1)) in the direction of `turn` of a full circle
/// (0x004c1620).
inline glm::vec2 fallPoint(const glm::vec2& centre, float radius, float fraction, float turn) {
    const float angle = turn * 6.2831855f;
    return {centre.x + fraction * std::cos(angle) * radius, centre.y + radius * std::sin(angle) * fraction};
}

/// 0x007fc220: where one falls to, from `centre` (the object's place) and
/// its point in the plane. `segment(from, to)` is the world's intersect
/// (0x007a3b70, flags 0x120111): how far along the first surface is, 0..1,
/// or none. The way up is slanted, from just above the object to the point
/// kRise above it, and 0.95 of the way to a ceiling is kept; from there the
/// first surface within kDrop straight down. None where there is none - the
/// object's own place is then used.
template <class Segment>
std::optional<glm::vec3> landing(const glm::vec3& centre, const glm::vec2& point, Segment&& segment) {
    const glm::vec3 from(centre.x, centre.y, centre.z + kCeilingFrom);
    glm::vec3 top(point, centre.z + kRise);
    if (const std::optional<float> up = segment(from, top)) top = from + (top - from) * (*up * kCeilingKeep);
    const glm::vec3 bottom(top.x, top.y, top.z - kDrop);
    if (const std::optional<float> down = segment(top, bottom)) return top + (bottom - top) * *down;
    return std::nullopt;
}

/// 0x007fc220: where one falls, in the frame its effect is in - a
/// transport's (+0x144, while +0x184 is set, 0x007fa1b0) or else the
/// world's. The object's place (`centre`, in that frame) is put in the
/// world (0x004c21b0), the point drawn around it there, and the landing
/// sought there brought back into the frame (0x004c2fc0, 0x004c2300); where
/// it lands on nothing, the object's own place.
template <class Segment>
glm::vec3 drop(const glm::vec3& centre, const glm::mat4* frame, float radius, float fraction, float turn,
               Segment&& segment) {
    const glm::vec3 world = frame ? glm::vec3(*frame * glm::vec4(centre, 1.0f)) : centre;
    const std::optional<glm::vec3> landed = landing(world, fallPoint(glm::vec2(world), radius, fraction, turn), segment);
    if (!landed) return centre;
    return frame ? glm::vec3(glm::inverse(*frame) * glm::vec4(*landed, 1.0f)) : *landed;
}

/// 0x007fb2d0: where one is drawn - its place, put in the world by the
/// frame where there is one.
inline glm::vec3 drawnAt(const glm::vec3& place, const glm::mat4* frame) {
    return frame ? glm::vec3(*frame * glm::vec4(place, 1.0f)) : place;
}

/// 0x007fc220: how long one waits before it plays, seconds - a random byte
/// (`fraction` of 255, rounded), twice, in ms.
inline float startDelaySeconds(float fraction) {
    const uint32_t ms = (static_cast<uint32_t>(std::lrint(fraction * 255.0f)) & 0xFFu) * 2u;
    return static_cast<float>(ms) * 0.001f;
}

}  // namespace wowee::rendering::blizzard_object
