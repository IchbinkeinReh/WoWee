#pragma once

#include <cstddef>
#include <cstdint>

#include "game/spell_defines.hpp"

namespace wowee {
namespace game {

/// Who caused a line, as 0x007190a0 answers it: the active player itself, a
/// unit of the player's (its CHARMEDBY, else CREATEDBY, is the player, or is
/// a unit whose own is - a pet's totem or guardian), or neither.
enum class FloaterSource : uint8_t { Other, Player, PlayersUnit };

/// The world-text kinds the client's floaters are drawn as (0x007e6030's
/// second argument): a mechanic 0, a crit 2, the miss table's 3 (absorb 1,
/// no-text 11), experience 4, honour 5, a heal 6 or a crit heal 7, threat 10.
enum class FloaterKind : uint8_t {
    Mechanic = 0, Absorb = 1, Crit = 2, Miss = 3, Experience = 4, Honor = 5,
    Heal = 6, CritHeal = 7, Threat = 10, None = 11,
};

/// The settings a damage, miss or heal floater over the world reads
/// (0x0071f990, 0x00722340, 0x00722440).
struct FloaterSettings {
    bool combatDamage = true;           ///< CombatDamage
    bool periodicSpells = true;         ///< CombatLogPeriodicSpells
    bool petMeleeDamage = true;         ///< PetMeleeDamage
    bool petSpellDamage = true;         ///< PetSpellDamage
    bool combatHealing = true;          ///< CombatHealing
};

/// Whether a combat text line is drawn over the unit it landed on.
///
/// Never over the active player (each of the three returns first for it),
/// and only for a source 0x007190a0 claims. A miss and its kin (0x0071f990)
/// and damage (0x00722340) need CombatDamage, a periodic one also
/// CombatLogPeriodicSpells, and a unit of the player's also PetMeleeDamage
/// for a swing (no spell, or one with AttributesEx3 0x8000) or
/// PetSpellDamage for a spell. Healing (0x00722440) is the player's own
/// alone, under CombatHealing. Nothing else in the combat text is a floater:
/// what happens to the character is Blizzard_CombatText's.
inline bool worldFloaterShown(CombatTextEntry::Type type, FloaterSource source, bool onActivePlayer,
                              bool periodic, bool meleeLike, const FloaterSettings& s) {
    using T = CombatTextEntry;
    if (onActivePlayer || source == FloaterSource::Other) return false;
    switch (type) {
        case T::MISS: case T::RESIST: case T::DODGE: case T::PARRY: case T::BLOCK:
        case T::EVADE: case T::IMMUNE: case T::DEFLECT: case T::ABSORB: case T::REFLECT:
        case T::MELEE_DAMAGE: case T::SPELL_DAMAGE: case T::CRIT_DAMAGE: case T::PERIODIC_DAMAGE:
        case T::GLANCING: case T::CRUSHING: {
            if (!s.combatDamage) return false;
            if ((periodic || type == T::PERIODIC_DAMAGE) && !s.periodicSpells) return false;
            if (source == FloaterSource::PlayersUnit)
                return meleeLike ? s.petMeleeDamage : s.petSpellDamage;
            return true;
        }
        case T::HEAL: case T::CRIT_HEAL: case T::PERIODIC_HEAL:
            return source == FloaterSource::Player && s.combatHealing;
        default:
            return false;
    }
}

/// The world-text kind a floater is drawn as: damage 0 and a crit 2
/// (0x00722340), a heal 6 and a crit heal 7 (0x00722440), and for a miss
/// and its kin the table at 0x00a34c20 - absorb 1, the rest 3 (0x0071f990).
inline uint8_t floaterKindOf(CombatTextEntry::Type type) {
    using T = CombatTextEntry;
    switch (type) {
        case T::CRIT_DAMAGE: return static_cast<uint8_t>(FloaterKind::Crit);
        case T::HEAL: case T::PERIODIC_HEAL: return static_cast<uint8_t>(FloaterKind::Heal);
        case T::CRIT_HEAL: return static_cast<uint8_t>(FloaterKind::CritHeal);
        case T::ABSORB: return static_cast<uint8_t>(FloaterKind::Absorb);
        case T::MISS: case T::RESIST: case T::DODGE: case T::PARRY: case T::BLOCK:
        case T::EVADE: case T::IMMUNE: case T::DEFLECT: case T::REFLECT:
            return static_cast<uint8_t>(FloaterKind::Miss);
        default: return 0;
    }
}

/// The colour a damage or miss floater's maker gives it (0x00722340,
/// 0x0071f990): a swing (no spell, or AttributesEx3 0x8000) the player's own
/// none - its kind's (world_text::colourOf) - and one of the player's unit's
/// 0xffff8400; anything a spell did 0xffffde00. A heal gives none
/// (0x00722440). Zero for none.
inline uint32_t floaterColour(CombatTextEntry::Type type, FloaterSource source, bool meleeLike) {
    using T = CombatTextEntry;
    switch (type) {
        case T::MISS: case T::RESIST: case T::DODGE: case T::PARRY: case T::BLOCK:
        case T::EVADE: case T::IMMUNE: case T::DEFLECT: case T::ABSORB: case T::REFLECT:
        case T::MELEE_DAMAGE: case T::SPELL_DAMAGE: case T::CRIT_DAMAGE: case T::PERIODIC_DAMAGE:
        case T::GLANCING: case T::CRUSHING:
            if (!meleeLike) return world_text::kSpellColour;
            return source == FloaterSource::PlayersUnit ? world_text::kPetSwingColour : 0u;
        default:
            return 0u;
    }
}

/// The global string 0x0071f990 shows for a miss and its kin - the table at
/// 0x00adbfdc - or nullptr for a line that is not one.
inline const char* missFloaterKey(CombatTextEntry::Type type) {
    using T = CombatTextEntry;
    switch (type) {
        case T::MISS: return "COMBAT_TEXT_MISS";
        case T::RESIST: return "COMBAT_TEXT_RESIST";
        case T::DODGE: return "COMBAT_TEXT_DODGE";
        case T::PARRY: return "COMBAT_TEXT_PARRY";
        case T::BLOCK: return "COMBAT_TEXT_BLOCK";
        case T::EVADE: return "COMBAT_TEXT_EVADE";
        case T::IMMUNE: return "COMBAT_TEXT_IMMUNE";
        case T::DEFLECT: return "COMBAT_TEXT_DEFLECT";
        case T::ABSORB: return "COMBAT_TEXT_ABSORB";
        case T::REFLECT: return "COMBAT_TEXT_REFLECT";
        default: return nullptr;
    }
}

/// A victim's threat status on a unit's list as the client keeps it (+0x28 of
/// the entry): 0 off the list; the unit's top (0x00737620) 4, or 3 while
/// another entry stands at 2; anyone else 2 with threat at least the top's,
/// else 1 (0x0071c3b0: the percentage of the top's threat, 100 when the top
/// has none). `threats` is the list highest first.
inline int clientThreatStatus(const uint32_t* threats, size_t count, size_t index) {
    if (index >= count) return 0;
    const uint32_t top = threats[0];
    const auto pct = [&](size_t i) -> int64_t {
        if (top == 0) return 100;
        const int64_t v = static_cast<int64_t>(threats[i]) * 100 / static_cast<int64_t>(top);
        return v > 250 ? 250 : v;
    };
    if (index != 0) return pct(index) >= 100 ? 2 : 1;
    for (size_t i = 1; i < count; ++i)
        if (pct(i) >= 100) return 3;
    return 4;
}

/// Whether the threat floater (0x00719220) shows for a status moving from
/// `before` to `now` (its callers raise it only on a change): not for 2 -> 1
/// nor 3 -> 4. Its text is the global string COMBAT_THREAT_INCREASE_<now>
/// where `now` is above `before`, else COMBAT_THREAT_DECREASE_<now>.
constexpr bool threatFloaterShown(int before, int now) {
    if (before == 2 && now == 1) return false;
    if (before == 3 && now == 4) return false;
    return true;
}

/// The mechanic floater's gate (0x0074da00): fctAllSpellMechanics, or
/// fctSpellMechanics with the record concerning the player (0x0074d430) or
/// fctSpellMechanicsOther.
constexpr bool mechanicFloaterShown(bool all, bool mechanics, bool mechanicsOther, bool concernsPlayer) {
    return all || (mechanics && (concernsPlayer || mechanicsOther));
}

} // namespace game
} // namespace wowee
