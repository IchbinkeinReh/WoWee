#pragma once

// What a melee blow shows, and when.
//
// SMSG_ATTACKERSTATEUPDATE (0x00756800 case 0x14a) is logged as it arrives
// (0x007512b0) and kept on its attacker (0x00755050, +0xb98). The attacker's model
// shows it as its swing passes its events (0x00756240):
//
//   $CPP  the victim parries, dodges or blocks, and is then not wounded by it
//         (hit info 0x2 off);
//   $CAH, $AH0..$AH3
//         it lands (0x00755e40): settled, then the victim's wound and blood.
//
// Settled (0x00755a60) it is one text over the victim and UNIT_COMBAT. A blow whose
// attacker is not in the world lands as it arrives; one kept and never landed is
// settled when the attacker's next arrives or its attack stops.

#include "game/spell_defines.hpp"

#include <cstdint>
#include <optional>

namespace wowee::game::melee_landing {

struct Blow {
    uint64_t attacker = 0;
    uint64_t target = 0;
    uint32_t hitInfo = 0;
    int32_t damage = 0;
    uint32_t victimState = 0;
    uint32_t meleeSpellId = 0;
    uint32_t blocked = 0;
    int32_t school = 0;  ///< The first sub-damage's mask (+0x1c), UNIT_COMBAT's.
};

constexpr uint32_t kAffectsVictim = 0x2;
constexpr uint32_t kFullAbsorb = 0x20;
constexpr uint32_t kFullResist = 0x80;
constexpr uint32_t kCrit = 0x200;
constexpr uint32_t kSilentMiss = 0x4000;
constexpr uint32_t kCrushing = 0x8000;
constexpr uint32_t kGlancing = 0x10000;
constexpr uint32_t kCrushed = 0x20000;
constexpr uint32_t kNoDamageHit = 0x1000000;

/// Whether settling says anything of it: of a melee spell's only where it hit.
constexpr bool said(const Blow& b) { return b.meleeSpellId == 0 || b.victimState == 1; }

/// The text over the victim, one a blow: how it was avoided, else nothing done - an
/// absorb, a resist or a miss, not on the active player - else the damage.
constexpr std::optional<CombatTextEntry::Type> text(const Blow& b, bool onActivePlayer) {
    using T = CombatTextEntry;
    if (!said(b)) return std::nullopt;
    switch (b.victimState) {
        case 2: return T::DODGE;
        case 3: return T::PARRY;
        case 5: return T::BLOCK;
        case 6: return T::EVADE;
        case 7: return T::IMMUNE;
        case 8: return T::DEFLECT;
        default: break;
    }
    if (b.damage == 0) {
        if (onActivePlayer || (b.hitInfo & (kSilentMiss | kNoDamageHit)) != 0) return std::nullopt;
        if ((b.hitInfo & kFullAbsorb) != 0) return T::ABSORB;
        return (b.hitInfo & kFullResist) != 0 ? T::RESIST : T::MISS;
    }
    if ((b.hitInfo & kCrit) != 0) return T::CRIT_DAMAGE;
    if ((b.hitInfo & kCrushed) != 0) return T::CRUSHING;
    return (b.hitInfo & kGlancing) != 0 ? T::GLANCING : T::MELEE_DAMAGE;
}

/// What the victim does of it at $CPP.
enum class Reaction : uint8_t { None, Parry, Dodge, Block };
constexpr Reaction reaction(const Blow& b) {
    if (b.victimState == 3) return Reaction::Parry;
    if (b.victimState == 2 || b.victimState == 8) return Reaction::Dodge;
    return b.blocked != 0 ? Reaction::Block : Reaction::None;
}

/// Whether it wounds as it lands, and whether the wound bleeds (0x00755e40).
constexpr bool wounds(const Blow& b) { return (b.hitInfo & kAffectsVictim) != 0; }
constexpr bool bleeds(const Blow& b) {
    return wounds(b) && (b.damage != 0 || (b.hitInfo & kNoDamageHit) != 0) &&
           (b.victimState == 1 || b.victimState == 4);
}

}  // namespace wowee::game::melee_landing
