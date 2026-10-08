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
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering::spell_kit {

/// The kinds 0x00745230 switches on (the request's +8).
enum class KitType : uint32_t {
    Precast = 0,
    Cast = 1,
    State = 2,        ///< an aura's StateKit (0x00724820)
    Impact = 3,
    StateDone = 8,    ///< an aura's StateDoneKit as it goes (0x0071e930)
};

/// Whether a kit's models stay until removed: a state kit's callback
/// (0x007449c0) loops them, the others' (0x00744870) play them once.
constexpr bool kitLoops(KitType type) { return type == KitType::State; }

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
