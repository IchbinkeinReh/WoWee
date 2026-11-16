#pragma once

// Whether a unit's cast shows as one the player cannot interrupt: the shield
// on the cast bar, UnitCastingInfo / UnitChannelInfo's notInterruptible and
// UNIT_SPELLCAST_INTERRUPTIBLE / UNIT_SPELLCAST_NOT_INTERRUPTIBLE.
//
// The client works it out from what the player knows (0x0053ca70: the
// schools of the player's spells that interrupt a cast or silence) against
// the caster's immunities (0x007262e0), keeps the answer on the unit
// (Unit_C +0xa34 bit 0x8) and reports it only while the player knows some
// interrupt at all (0x0071ab20).
//
// The spell record fields, by their offsets in the client's spell record and
// the Spell.dbc columns its loader (0x008a65e0) fills them from - the four
// localized strings each take one pointer there (+0x220..+0x22c) for the 68
// columns 136..203, so a field past them sits 64 columns further on than
// its offset / 4:
//   +0x02c AttributesEx7          column 11
//   +0x11c Effect[3]              columns 71-73
//   +0x17c EffectApplyAuraName[3] columns 95-97
//   +0x1b8 EffectMiscValue[3]     columns 110-112
//   +0x258 PreventionType         column 214
//   +0x284 SchoolMask             column 225

#include <cstdint>
#include <vector>

namespace wowee::game::cast_interrupt {

/// What 0x0053ca70 and 0x007262e0 read of a spell.
struct SpellFields {
    uint32_t attrEx7 = 0;           ///< +0x2c
    uint32_t effect[3] = {0, 0, 0};    ///< +0x11c
    uint32_t auraName[3] = {0, 0, 0};  ///< +0x17c
    uint32_t miscValue[3] = {0, 0, 0}; ///< +0x1b8
    uint32_t preventionType = 0;    ///< +0x258
    uint32_t schoolMask = 0;        ///< +0x284
};

/// The four masks 0x0053ca70 builds from the player's spells (0x00be8da8,
/// 0x00be8dac, 0x00be8db0, 0x00be8db4).
struct Masks {
    uint32_t interrupt = 0;           ///< schools of effect 68 (interrupt cast)
    uint32_t interruptNonPlayer = 0;  ///< schools of AttributesEx7 0x800 spells
    uint32_t silence = 0;             ///< schools of effect 6 applying aura 27 (silence)
    uint32_t silenceNonPlayer = 0;    ///< schools of AttributesEx7 0x1000 spells

    /// Any of them that counts against a caster: the non-player masks do not
    /// count against a player (OBJECT_FIELD_TYPE 0x10).
    [[nodiscard]] bool any(bool casterIsPlayer) const {
        return interrupt != 0 || (casterIsPlayer ? 0u : interruptNonPlayer) != 0 || silence != 0 ||
               (casterIsPlayer ? 0u : silenceNonPlayer) != 0;
    }
};

/// One of the player's spells added to the masks (0x0053ca70's last loop).
inline void addKnownSpell(Masks& m, const SpellFields& s) {
    for (int i = 0; i < 3; ++i) {
        if (s.effect[i] == 6) {
            if (s.auraName[i] == 0x1b) m.silence |= s.schoolMask;
        } else if (s.effect[i] == 0x44) {
            m.interrupt |= s.schoolMask;
        }
    }
    if ((s.attrEx7 & 0x800u) != 0) m.interruptNonPlayer |= s.schoolMask;
    if ((s.attrEx7 & 0x1000u) != 0) m.silenceNonPlayer |= s.schoolMask;
}

/// One aura on the caster: its spell and which of its effects are in force
/// (the aura's flags & 7, the slot's +0xc).
struct Aura {
    SpellFields spell;
    uint8_t effectMask = 0;
};

/// The caster as 0x007262e0 sees it.
struct Caster {
    bool isPlayer = false;          ///< OBJECT_FIELD_TYPE has 0x10
    uint32_t schoolImmunity = 0;    ///< +0xa64, from SMSG_SPELL_START cast flag 0x4000000
    uint32_t mechanicImmunity = 0;  ///< +0xa68, the same
    std::vector<Aura> auras;
};

/// 0x007262e0's two answers: the bit it leaves on the unit and what it
/// returns.
struct Outcome {
    bool lock = false;
    bool result = false;
};

/// 0x007262e0: the bit is cleared, then set when no interrupt the player
/// knows can reach the cast. `havePlayer` is whether the active player
/// object exists.
[[nodiscard]] inline Outcome evaluate(const Masks& m, bool havePlayer, const SpellFields& cast, const Caster& c) {
    // AttributesEx7 0x2000: always interruptible.
    if ((cast.attrEx7 & 0x2000u) != 0) return {false, false};
    // Only a cast a silence prevents (PreventionType 1) can be stopped.
    if (!havePlayer || cast.preventionType != 1) return {true, true};
    uint32_t intr = m.interrupt;
    uint32_t intrNp = c.isPlayer ? 0u : m.interruptNonPlayer;
    uint32_t sil = m.silence;
    uint32_t silNp = c.isPlayer ? 0u : m.silenceNonPlayer;
    // Nothing the player knows interrupts: the bit stays clear.
    if (intr == 0 && intrNp == 0 && sil == 0 && silNp == 0) return {false, true};
    // Immune to the interrupt mechanic (26) or the silence mechanic (9), or
    // to the schools.
    if ((c.mechanicImmunity & 0x4000000u) != 0) {
        intr = 0;
        intrNp = 0;
    } else if (c.schoolImmunity != 0) {
        intr &= ~c.schoolImmunity;
        intrNp &= ~c.schoolImmunity;
    }
    if ((c.mechanicImmunity & 0x200u) != 0) {
        sil = 0;
        silNp = 0;
    } else if (c.schoolImmunity != 0) {
        sil &= ~c.schoolImmunity;
        silNp &= ~c.schoolImmunity;
    }
    const auto anyLeft = [&] { return intr != 0 || intrNp != 0 || sil != 0 || silNp != 0; };
    if (anyLeft()) {
        // Only with an aura of school (39), mechanic (77), effect (37) or
        // state (38) immunity in force (0x005a1120 over +0xf20, which
        // 0x00727e70 builds from each aura's effects in force).
        bool immunityAura = false;
        for (const auto& a : c.auras) {
            for (int i = 0; i < 3; ++i) {
                if ((a.effectMask & (1u << i)) == 0) continue;
                const uint32_t t = a.spell.auraName[i];
                if (t == 0x27 || t == 0x4d || t == 0x25 || t == 0x26) immunityAura = true;
            }
        }
        if (immunityAura) {
            // Every effect of every aura, in force or not.
            for (const auto& a : c.auras) {
                if (!anyLeft()) break;
                for (int i = 0; i < 3; ++i) {
                    const uint32_t misc = a.spell.miscValue[i];
                    switch (a.spell.auraName[i]) {
                    case 0x25:  // effect immunity: interrupt cast
                        if (misc == 0x44) intr = intrNp = 0;
                        break;
                    case 0x26:  // state immunity: silence
                        if (misc == 0x1b) sil = silNp = 0;
                        break;
                    case 0x27:  // school immunity
                        intr &= ~misc;
                        intrNp &= ~misc;
                        sil &= ~misc;
                        silNp &= ~misc;
                        break;
                    case 0x4d:  // mechanic immunity: silence (9), interrupt (26)
                        if (misc == 9) sil = silNp = 0;
                        else if (misc == 0x1a) intr = intrNp = 0;
                        break;
                    default:
                        break;
                    }
                }
            }
        }
        if (anyLeft()) return {false, false};
    }
    return {true, true};
}

/// 0x0071ab20: what the unit's cast reports - the bit, while the player
/// knows something that interrupts.
[[nodiscard]] inline bool notInterruptible(bool lock, const Masks& m, bool casterIsPlayer) {
    return lock && m.any(casterIsPlayer);
}

}  // namespace wowee::game::cast_interrupt
