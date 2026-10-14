#pragma once

/// A dynamic object's model as the client makes it (DynamicObject_C).
///
/// The model (0x00705100 -> 0x00704b30): the object's spell
/// (DYNAMICOBJECT_SPELLID, the fields' +0xc) names its SpellVisual
/// (0x007fa290), whose PersistentAreaKit (+0x64) names a SpellVisualKit;
/// the kit's WorldEffect (+0x38) names the SpellVisualEffectName row whose
/// file is drawn, and a WorldEffect the table lacks gives way to the
/// BaseEffect (+0x14). No kit, no row: no model.
///
/// When the model has loaded (0x00704d90): its scale - see modelScale - and
/// its Birth (127) where it has one. Each time a sequence ends (0x00704af0
/// -> 0x00704a00) it plays its Hold (158) where it has one, else its Stand.
/// Its events go to 0x00704c30 (by 0x00704f20): $SND plays the data's
/// SoundEntries row once where the event is, $SHK shakes the camera by the
/// data's SpellEffectCameraShakes row.
///
/// Made (0x00705230 -> 0x00704f60): the kit's SoundID (+0x3c) plays on the
/// object, looping whatever its row says and rising over two seconds.
///
/// Gone (0x00704a70, the object's slot +0x10): taken away by
/// SMSG_DESTROY_OBJECT (0x004d7610 passes 1) a model that has a Decay (159)
/// plays it and the object goes as it ends (0x00704a40); out of range
/// (0x004d7230 passes 0), or without a Decay, it goes at once. As it goes
/// (0x00704940, 0x007054e0) its sound falls over three seconds.

#include <cstdint>

namespace wowee::rendering::dynamic_object {

inline constexpr uint32_t kAnimStand = 0;
inline constexpr uint32_t kAnimBirth = 127;
inline constexpr uint32_t kAnimHold = 158;
inline constexpr uint32_t kAnimDecay = 159;
/// The kit sound's rise (0x00704f60: the play's +0x10) and its fall as the
/// object goes (0x00704940: 0x004c6390(+0x15c, 0, 3.0)).
inline constexpr float kSoundFadeInSeconds = 2.0f;
inline constexpr float kSoundFadeOutSeconds = 3.0f;

/// DYNAMICOBJECT_BYTES' low byte: an area spell's (1) and a far sight's (2)
/// model keeps the effect's own size.
constexpr bool sizedByRadius(uint8_t type) { return type != 1 && type != 2; }

/// 0x00704b30: the SpellVisualEffectName row the kit's model is, 0 for none
/// - its WorldEffect where the table has that row, else its BaseEffect.
constexpr uint32_t effectRow(uint32_t worldEffect, bool worldEffectFound, uint32_t baseEffect, bool baseEffectFound) {
    if (worldEffect != 0 && worldEffectFound) return worldEffect;
    return baseEffectFound ? baseEffect : 0u;
}

/// 0x00704d90: the model's scale (+0x154), which multiplies the object's
/// own (+0x9c). From 1: unless the object is an area spell or a far sight,
/// DYNAMICOBJECT_RADIUS over the model's vertex radius (the M2 header's
/// +0xb8) - over the effect's AreaEffectSize (+0xc) where the model's is not
/// above 0.001, and left at 1 where that is not above 0 either; then times
/// the effect's Scale (+0x10) where that is above 0. Without an effect row
/// the radius over the model's still applies, and nothing else.
constexpr float modelScale(uint8_t type, float radius, float modelRadius, bool haveEffect, float areaEffectSize,
                           float effectScale) {
    float scale = 1.0f;
    if (sizedByRadius(type)) {
        if (modelRadius > 0.001f) {
            scale = radius / modelRadius;
        } else {
            if (!haveEffect) return scale;
            if (areaEffectSize > 0.0f) scale = radius / areaEffectSize;
        }
    }
    if (haveEffect && effectScale > 0.0f) scale *= effectScale;
    return scale;
}

/// The sequence a loaded model starts with (0x00704d90): its Birth where it
/// has one, else what it plays of itself, its Stand.
constexpr uint32_t firstSequence(bool hasBirth) { return hasBirth ? kAnimBirth : kAnimStand; }
/// What follows each sequence's end (0x00704a00): its Hold where it has one,
/// else its Stand.
constexpr uint32_t idleSequence(bool hasHold) { return hasHold ? kAnimHold : kAnimStand; }
/// 0x00704a70: whether the model plays its Decay before the object goes -
/// only one destroyed, not one out of range, and only with a Decay.
constexpr bool decaysOnRemoval(bool destroyed, bool hasDecay) { return destroyed && hasDecay; }

}  // namespace wowee::rendering::dynamic_object
