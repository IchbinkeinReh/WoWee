#pragma once

/// SpellVisualKit as the client plays it on a unit, apart from the renderer.
///
/// 0x00745230 plays a kit: each of its model columns goes on the unit at its
/// own attachment (0x00744790 -> CEffect, 0x006f94c0), its sound plays, and
/// the unit's own part follows (0x0073b140: its animation, its CharProc
/// procedures, its camera shake). A kit's type says how its models play: a
/// state kit (2, an aura's, 0x00724820) loops until the aura goes
/// (0x0071e930 removes the spell's effects, 0x00743b40), every other kind
/// plays once.

#include <array>
#include <cstdint>
#include <optional>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering::spell_kit {

/// The kinds 0x00745230 switches on (the request's +8), by who asks.
enum class KitType : uint32_t {
    PlayImpact = 0,  ///< SMSG_PLAY_SPELL_IMPACT's kit (0x00800610)
    /// A cast kit (0x0080e1b0), a unit's impact kit (0x00801f10, 0x00700e20)
    /// and SMSG_PLAY_SPELL_VISUAL's kit (0x008006c0).
    Cast = 1,
    State = 2,       ///< an aura's StateKit (0x00724820)
    Area = 3,        ///< an area kit at a place (0x0080e1b0, 0x00700e20)
    Precast = 4,     ///< the PrecastKit while the cast runs (0x007fa2e0)
    StateDone = 8,   ///< an aura's StateDoneKit as it goes (0x0071e930)
};

/// Whether a kit's models stay until removed: a state kit's callback
/// (0x007449c0) loops them, the others' (0x00744870) play them once.
constexpr bool kitLoops(KitType type) { return type == KitType::State; }

/// How a kit's model plays, by the callback 0x00745230 gives it.
enum class KitModelLife {
    Once,    ///< 0x00744870: its Stand once, then its Decay where it has one, then gone
    Repeat,  ///< 0x007435a0: its animation again each time it ends, until removed
    Hold,    ///< 0x007449c0: its Stand once, then its Hold (or Stand) until removed
};
constexpr KitModelLife kitModelLife(KitType type) {
    switch (type) {
        case KitType::State: return KitModelLife::Hold;
        case KitType::Precast: return KitModelLife::Repeat;
        default: return KitModelLife::Once;
    }
}

/// Which of a kit's models a model is, as 0x00745230 treats it.
enum class KitModelKind : uint8_t {
    Head,       ///< HeadEffect (+0x0c), played first and by every kit
    Base,       ///< BaseEffect (+0x14)
    Column,     ///< the other model columns
    World,      ///< WorldEffect (+0x38)
    AttachRow,  ///< a SpellVisualKitModelAttach row (0x007fa9f0)
};

/// Where a kit's model goes.
enum class KitModelPlace {
    None,        ///< not played
    Attachment,  ///< on the unit at its attachment (0x00744790)
    AtUnit,      ///< in the world where the unit stands (0x006f7c40, flag 0x200)
    AtPlace,     ///< in the world at the place the request names (+0x0c)
};

/// 0x00745230: an area kit (3, 7: flag 0x2000) plays its HeadEffect and its
/// WorldEffect, and its BaseEffect and model-attach rows only at a place it
/// was given; every other kit plays each model, at the place where it has
/// one, else on the unit - at the model's attachment, or where the unit
/// stands for the WorldEffect and a row without an attachment.
constexpr KitModelPlace kitModelPlace(KitType type, KitModelKind kind, int32_t attachment, bool hasPlace) {
    const bool area = type == KitType::Area || static_cast<uint32_t>(type) == 7;
    if (area && kind != KitModelKind::Head && kind != KitModelKind::World) {
        const bool atPlace = kind == KitModelKind::Base || kind == KitModelKind::AttachRow;
        return atPlace && hasPlace ? KitModelPlace::AtPlace : KitModelPlace::None;
    }
    if (hasPlace) return KitModelPlace::AtPlace;
    if (kind == KitModelKind::World || attachment < 0) return KitModelPlace::AtUnit;
    return KitModelPlace::Attachment;
}

/// 0x006f7950: a unit's size for the effects it places in the world - 0.3
/// of the narrower side of its model's box (the M2 header's +0xa0), at least
/// 1, times its CreatureModelData WorldEffectScale (+0x5c).
inline float unitWorldEffectSize(float worldEffectScale, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    float extent = boxMax.y - boxMin.y;
    if (boxMax.x - boxMin.x < extent) extent = boxMax.x - boxMin.x;
    const float size = extent * 0.3f;
    return (size <= 1.0f ? 1.0f : size) * worldEffectScale;
}

/// 0x006f8ae0: a world-placed kit model's scale - the unit's size and scale
/// where it stands at the unit (`unitSize`, 1 at a place), times the
/// effect's Scale, held within its allowed scales; 1 where that is not
/// above 0.
constexpr float worldKitModelScale(float unitSize, float effectScale, float minScale, float maxScale) {
    float scale = unitSize * effectScale;
    if (scale < minScale) scale = minScale;
    else if (!(scale < maxScale)) scale = maxScale;
    return scale > 0.0f ? scale : 1.0f;
}

/// 0x006f8ae0 with 0x006f84f0: a world-placed kit model's transform - at
/// `position`, turned to `facing` about Z, a model-attach row's offset and
/// turn (`local`) taken in that facing, at `scale`.
inline glm::mat4 worldKitModelMatrix(const glm::vec3& position, float facing, const glm::mat4& local, float scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, facing, glm::vec3(0.0f, 0.0f, 1.0f));
    return m * local * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
}

/// SpellVisual's kits for a cast's targets (0x00800d00): CasterImpactKit
/// (+0x38) on the caster, TargetImpactKit (+0x3c) on any other, each the
/// ImpactKit (+0x0c) where the visual has none.
constexpr uint32_t impactKitFor(bool onCaster, uint32_t impactKit, uint32_t casterImpactKit, uint32_t targetImpactKit) {
    const uint32_t kit = onCaster ? casterImpactKit : targetImpactKit;
    return kit != 0 ? kit : impactKit;
}

/// One-shot timing (0x00744870): when a model's Stand has run once it plays
/// its Decay, where it has one, and goes when that ends.
struct OnceTiming {
    float switchAt = 0.0f;  ///< seconds: the Stand's end
    bool decays = false;    ///< then its Decay
};
constexpr OnceTiming onceTiming(float standMs, bool hasDecay) {
    return OnceTiming{.switchAt = standMs * 0.001f, .decays = hasDecay};
}

/// A model column of SpellVisualKit and the M2 attachment 0x00745230 hangs
/// it on (0x00744790's first argument), in the order it plays them.
struct KitSlot {
    const char* column;
    uint32_t fallbackField;  ///< the 3.3.5a column, for a layout without the name
    uint32_t attachment;
};
inline constexpr std::array<KitSlot, 9> kKitSlots = {{
    {.column = "HeadEffect", .fallbackField = 3, .attachment = 20},       // +0x0c
    {.column = "BaseEffect", .fallbackField = 5, .attachment = 19},       // +0x14
    {.column = "LeftHandEffect", .fallbackField = 6, .attachment = 21},   // +0x18
    {.column = "RightHandEffect", .fallbackField = 7, .attachment = 22},  // +0x1c
    {.column = "BreathEffect", .fallbackField = 8, .attachment = 17},     // +0x20
    {.column = "ChestEffect", .fallbackField = 4, .attachment = 34},      // +0x10
    {.column = "SpecialEffect0", .fallbackField = 11, .attachment = 23},  // +0x2c
    {.column = "SpecialEffect1", .fallbackField = 12, .attachment = 24},  // +0x30
    {.column = "SpecialEffect2", .fallbackField = 13, .attachment = 25},  // +0x34
}};

/// 0x006f84f0: where a SpellVisualKitModelAttach row puts its model in its
/// attachment's frame - turned by Roll about X, then Pitch about Y, then the
/// negated Yaw about Z (row-vector multiplies onto an identity), at
/// (OffsetX, -OffsetY, OffsetZ).
inline glm::mat4 modelAttachMatrix(const glm::vec3& offset, float yaw, float pitch, float roll) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(offset.x, -offset.y, offset.z));
    if (yaw != 0.0f) m = glm::rotate(m, -yaw, glm::vec3(0.0f, 0.0f, 1.0f));
    if (pitch != 0.0f) m = glm::rotate(m, pitch, glm::vec3(0.0f, 1.0f, 0.0f));
    if (roll != 0.0f) m = glm::rotate(m, roll, glm::vec3(1.0f, 0.0f, 0.0f));
    return m;
}
/// 0x006f84f0: where a SpellVisualKitModelAttach row puts its model in its

/// SpellVisualKit Flags (+0x94) 0x20: a state kit's model plays its Stand
/// rather than its Hold (0x007449c0).
inline constexpr uint32_t kKitFlagStateStand = 0x20;
/// The animations a kit's model plays: a state kit's Hold (158) where the
/// model has one (0x007449c0), a one-shot kit's Decay (159) (0x00744870).
inline constexpr uint32_t kAnimHold = 158;
inline constexpr uint32_t kAnimDecay = 159;

/// SpellVisualKit CharProc (+0x44, four of them, with CharParamZero..Three
/// at +0x54, +0x64, +0x74, +0x84) as 0x007265c0 runs them on the unit.
/// 1: the unit's colour - ParamZero as 0xRRGGBB - while the effect lasts
///    (an aura's, until 0x0071e930 takes it off);
/// 13: the colour held for ParamOne seconds, then faded out over ParamTwo.
inline constexpr uint32_t kCharProcColour = 1;
inline constexpr uint32_t kCharProcColourFade = 13;

/// 6: the world's light tinted ParamZero (0xRRGGBB) for the spell's cast
///    time, reaching it at ParamOne of that time (0x007fa450);
/// 14: the unit's alpha, ParamZero in (0, 1], faded to over ParamTwo
///    seconds (or one) while the effect lasts (0x0071abe0).
inline constexpr uint32_t kCharProcLightTint = 6;
inline constexpr uint32_t kCharProcAlpha = 14;

/// 0 and 12: a SpellChainEffects chain (ParamZero, chopped) from the unit to
///    its targets, held by the effect while ParamTwo is set and all from the
///    unit while ParamThree is (0x007fc5f0, spell_chain.hpp).
inline constexpr uint32_t kCharProcChain = 0;
inline constexpr uint32_t kCharProcChainToo = 12;
constexpr bool isChainProc(uint32_t proc) { return proc == kCharProcChain || proc == kCharProcChainToo; }

/// 8: a swing trail on each weapon in the unit's hands, ParamZero its colour
///    under ParamThree's alpha, laid for ParamTwo ms (0x007265c0 case 8,
///    0x00715ba0, swing_trail.hpp).
inline constexpr uint32_t kCharProcSwingTrail = 8;

/// 11: the unit's animation held where it is - or ParamZero seconds into
///    its sequence, chopped to ms (0x00407930), no further than its length -
///    while the effect lasts (0x006f80b0, 0x00735bb0, 0x00735dd0).
inline constexpr uint32_t kCharProcFreeze = 11;
inline std::optional<float> freezeAtMs(float paramZero) {
    if (!(paramZero > 0.0f)) return std::nullopt;
    return static_cast<float>(static_cast<int32_t>(paramZero * 1000.0f));
}

/// 15: the unit faded over half a second to its own alpha times ParamZero,
///    chopped to a whole byte (0x00407930) - so to nothing or to all - held
///    ParamThree ms more, then back to its own over ParamTwo ms; only where
///    ParamOne, chopped, is below 12 (0x0071a940, 0x0073dab0).
inline constexpr uint32_t kCharProcTimedAlpha = 15;
struct TimedAlpha {
    float alpha = 1.0f;
    uint32_t inMs = 500;
    uint32_t endMs = 0;   ///< when it goes back
    uint32_t backMs = 0;  ///< over how long
};
inline std::optional<TimedAlpha> timedAlpha(const std::array<float, 4>& param, float unitAlpha, uint32_t nowMs) {
    const auto chop = [](float v) { return static_cast<int32_t>(v); };
    if (static_cast<uint32_t>(chop(param[1])) >= 12u) return std::nullopt;
    const auto byte = static_cast<uint8_t>(chop(unitAlpha * param[0]));
    // 0x00744030 takes that byte for an alpha and makes a byte of it again.
    const auto alpha = static_cast<uint8_t>(static_cast<int32_t>(static_cast<float>(byte) * 255.0f));
    TimedAlpha t;
    t.alpha = static_cast<float>(alpha) / 255.0f;
    t.endMs = nowMs + t.inMs + static_cast<uint32_t>(chop(param[3]));
    t.backMs = static_cast<uint32_t>(chop(param[2]));
    return t;
}
constexpr bool timedAlphaOver(const TimedAlpha& t, uint32_t nowMs) {
    return static_cast<int32_t>(nowMs - t.endMs) >= 0;
}

/// 0x007265c0 cases 1 and 13 colour the unit unless it is a creature whose
/// cache row has type flag 0x40 (+0x964 +0xc) and the spell is aimed at
/// enemies (0x007fe1b0 answers 2).
constexpr bool kitColoursUnit(bool creatureTypeFlag40, uint32_t spellTargetKind) {
    return !creatureTypeFlag40 || spellTargetKind != 2;
}

/// 0x007265c0 case 14: whether ParamZero is an alpha it takes.
constexpr bool kitAlphaTaken(float alpha) { return alpha > 0.0f && alpha <= 1.0f; }
/// 0x0071abe0: how long the unit takes to reach its kit's alpha - ParamTwo
/// seconds, or one where that is 0 (and one back to its own once the kit
/// is gone).
constexpr uint32_t kitAlphaFadeMs(float paramTwo) {
    const auto ms = paramTwo > 0.0f ? static_cast<uint32_t>(paramTwo * 1000.0f + 0.5f) : 0u;
    return ms != 0 ? ms : 1000u;
}

/// 0x007fa450: the light tint's colour and times, from the kit's ParamZero
/// and ParamOne and the spell's cast time (0x007ff180).
struct LightTint {
    uint32_t colour = 0;  ///< 0xAARRGGBB, alpha forced to 0xff
    uint32_t startMs = 0;
    uint32_t peakMs = 0;     ///< start + cast time x ParamOne
    uint32_t holdEndMs = 0;  ///< start + cast time
    uint32_t endMs = 0;      ///< then 100 ms out
};
constexpr LightTint lightTint(uint32_t colourParam, float paramOne, uint32_t castTimeMs, uint32_t nowMs) {
    return LightTint{.colour = colourParam | 0xff000000u,
                     .startMs = nowMs,
                     .peakMs = nowMs + static_cast<uint32_t>(static_cast<float>(castTimeMs) * paramOne + 0.5f),
                     .holdEndMs = nowMs + castTimeMs,
                     .endMs = nowMs + castTimeMs + 100u};
}
/// 0x007f9e10: how far toward the tint the light is at `nowMs` - up to the
/// peak, held to the cast's end, then out; false once it is over.
constexpr bool lightTintAmount(const LightTint& tint, uint32_t nowMs, float& amount) {
    if (nowMs < tint.peakMs) {
        amount = static_cast<float>(nowMs - tint.startMs) / static_cast<float>(tint.peakMs - tint.startMs);
        return true;
    }
    if (nowMs < tint.holdEndMs) {
        amount = 1.0f;
        return true;
    }
    if (nowMs < tint.endMs) {
        amount = 1.0f - static_cast<float>(nowMs - tint.holdEndMs) / static_cast<float>(tint.endMs - tint.holdEndMs);
        return true;
    }
    return false;
}
/// 0x006acc50 on a float colour: `from` moved toward `to` by alpha/256, all
/// the way at 255 (the light's colours, 0x007f3230 and 0x007f0530).
inline glm::vec3 tintColour(const glm::vec3& from, const glm::vec3& to, uint32_t alpha) {
    if (alpha >= 0xffu) return to;
    return from + (to - from) * (static_cast<float>(alpha) / 256.0f);
}

/// 0x007265c0 case 13: the unit's colour fade (+0xb10 .. +0xb1c).
struct ColourFade {
    uint32_t startMs = 0;
    uint32_t colour = 0xFFFFFFFFu;  ///< 0xAARRGGBB, alpha forced to 0xff
    uint32_t holdMs = 0;
    uint32_t fadeMs = 0;
};

/// 0x006acc50: `from` moved toward `to` by alpha/256, a byte a channel.
constexpr uint32_t lerpColour(uint32_t from, uint32_t to, uint32_t alpha) {
    if (alpha == 0xff) return (from & 0xff000000u) | (to & 0x00ffffffu);
    uint32_t out = from & 0xff000000u;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        const uint32_t f = (from >> shift) & 0xffu;
        const uint32_t t = (to >> shift) & 0xffu;
        const uint32_t channel = ((((t - f) * alpha) >> 8) + f) & 0xffu;
        out |= channel << shift;
    }
    return out;
}

/// 0x0071a9a0: the fade's colour at `nowMs` over white, or nothing once it
/// has run out.
constexpr bool fadeColour(const ColourFade& fade, uint32_t nowMs, uint32_t& out) {
    const int32_t sinceHold = static_cast<int32_t>(nowMs - (fade.startMs + fade.holdMs));
    if (sinceHold < 0) {
        out = fade.colour;
        return true;
    }
    if (static_cast<int32_t>(nowMs - fade.fadeMs - (fade.startMs + fade.holdMs)) < 0) {
        const float t = static_cast<float>(sinceHold) / static_cast<float>(fade.fadeMs);
        const auto alpha = static_cast<int32_t>((1.0f - t) * 255.0f + 0.5f);
        out = 0xFFFFFFFFu;
        if (alpha != 0) out = lerpColour(out, fade.colour, static_cast<uint32_t>(alpha));
        return true;
    }
    return false;
}

/// 0x00720db0: the colour the unit's model multiplies its diffuse light by
/// (CM2Model +0x180, 0x00820000's +0x1a0).
inline glm::vec3 colourToRgb(uint32_t argb) {
    return glm::vec3(static_cast<float>((argb >> 16) & 0xffu), static_cast<float>((argb >> 8) & 0xffu),
                     static_cast<float>(argb & 0xffu)) * 0.003921569f;
}

/// SpellVisual Flags (+0x34) 0x8: the aura's state kit shows only while the
/// unit has its weapons away, casts nothing and is idle (0x00720400).
inline constexpr uint32_t kVisualFlagUnarmedStateKit = 0x8;

/// A unit's two bits for those kits (+0xa30): 0x2000, it has one of those
/// auras; 0x4000, their kit is showing.
struct UnarmedKitBits {
    bool any = false;
    bool shown = false;
};

/// What 0x00720400 does to those kits.
enum class UnarmedKitStep {
    None,
    HideAll,    ///< every such aura's kits are removed (0x00743b40)
    ShowFirst,  ///< the first such aura's state kit plays again
};

/// 0x00720400(show, force): `show` is the unit's 0x10000 with its weapons
/// away (+0xb5c 0) and no cast (+0xa60 0). Unforced it does nothing while
/// the kits already show as asked, or the unit has none of them.
constexpr UnarmedKitStep unarmedKitStep(UnarmedKitBits& bits, bool show, bool force, bool hasSuchAura) {
    if (!force && (show == bits.shown || !bits.any)) return UnarmedKitStep::None;
    bits.any = false;
    if (!show) {
        bits.shown = false;
        if (!hasSuchAura) return UnarmedKitStep::None;
        bits.any = true;
        return UnarmedKitStep::HideAll;
    }
    if (!hasSuchAura) return UnarmedKitStep::None;
    bits.any = true;
    if (bits.shown) return UnarmedKitStep::None;
    bits.shown = true;
    return UnarmedKitStep::ShowFirst;
}

}  // namespace wowee::rendering::spell_kit
