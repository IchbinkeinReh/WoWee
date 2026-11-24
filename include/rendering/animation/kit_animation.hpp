#pragma once

// What a spell visual kit played on a unit does to its animation: the unit's
// own part of 0x0073b140.
//
// 0x00745230 plays a kit on a unit and hands the unit the kit, its type and
// flags of its own (0x40 for a PrecastKit, 0x1000 for a state kit), and the
// unit plays the kit's StartAnimID (+4) or AnimID (+8) as any animation it is
// asked (0x007385c0) - or, for an AnimID among the wounds, the wound
// (0x00736640). A state kit's animation (type 2) is not played here: it
// overrides what the unit is asked while it is on (0x0071e400).

#include "rendering/animation/animation_data.hpp"

#include <cstdint>

namespace wowee {
namespace rendering {
namespace kit_animation {

/// The kit types 0x0073b140 tells apart (spell_kit::KitType's values).
inline constexpr uint32_t kTypeVisual = 0;
inline constexpr uint32_t kTypeState = 2;
inline constexpr uint32_t kTypePrecast = 4;

/// What 0x0073b140 reads of a kit played on a unit.
struct KitPlay {
    uint32_t type = 1;
    int32_t startAnim = -1;  ///< StartAnimID (+4)
    int32_t anim = -1;       ///< AnimID (+8)
    uint32_t kitFlags = 0;   ///< Flags (+0x94)
    /// The spell has a cast time (vtable +0x148 of the spell, 1 or more):
    /// a PrecastKit's AnimID is played only then.
    bool castTime = false;
    /// The AnimID of the spell's visual's CastKit (the visual's +8 kit),
    /// -1 for none: a PrecastKit's AnimID is not played over it.
    int32_t castKitAnim = -1;
};

struct Action {
    enum class Kind : uint8_t {
        None,
        Play,   ///< ask `anim` (0x007385c0)
        Start,  ///< ask the StartAnimID, and remember it (+0xa38 0x10000)
        Wound,  ///< 0x00736640, CombatCritical for `critical`
    };
    Kind kind = Kind::None;
    uint32_t anim = 0;
    /// Played once: a StartAnimID, a cast or impact kit's AnimID; a
    /// PrecastKit's AnimID is the cast's loop.
    bool once = true;
    /// The flags it is asked with (0x00714ce0's from the kit's Flags).
    uint32_t askFlags = 0;
    bool critical = false;
    /// The StartAnimID played before is no longer the cast's (+0xa38 0x10000
    /// cleared).
    bool clearsStart = false;
    /// A visual kit's animation (type 0, 0x80): the unit's cast loop
    /// waits while it plays (+0xfa4, 0x007224d0).
    bool holdsCastLoop = false;
};

/// 0x00714ce0: kit Flags 0x4 -> 0x2, 0x8 -> 0x4 (neither puts the unit in it
/// by its tier), 0x10 -> 0x8, 0x100 -> 0x20.
[[nodiscard]] uint32_t askFlags(uint32_t kitFlags);

/// Special1H and Special2H (57, 58) asked of a unit are the special for what it
/// holds (0x007385c0), unless asked with 0x2 or 0x8: SpecialUnarmed (118) with
/// nothing in either hand, Special2H with a two-handed weapon and nothing in the
/// other, else Special1H.
[[nodiscard]] constexpr uint32_t specialAttack(uint32_t anim, uint32_t askFlags, bool mainHeld, bool mainTwoHanded,
                                               bool offHeld) {
    constexpr uint32_t kSpecial1H = 57, kSpecial2H = 58, kSpecialUnarmed = 118;
    if ((askFlags & 0xau) != 0 || (anim != kSpecial1H && anim != kSpecial2H)) return anim;
    if (!mainHeld && !offHeld) return kSpecialUnarmed;
    return mainTwoHanded && !offHeld ? kSpecial2H : kSpecial1H;
}

/// 0x0073b140 for a kit played on a unit showing `shown` (0x00717260).
[[nodiscard]] Action onKitPlayed(const anim::AnimationDataTable& data, const KitPlay& kit, int32_t shown);

}  // namespace kit_animation
}  // namespace rendering
}  // namespace wowee
