#pragma once

/// The client's own refusal of a dispel, a mechanic dispel or a spellsteal
/// aimed at units with nothing it could remove (0x0080ac90 -> 0x00803500's
/// last test, each unit's auras read by 0x00728500). No CMSG_CAST_SPELL goes
/// out: the cast fails at once with SPELL_FAILED_NOTHING_TO_DISPEL (0x56)
/// or SPELL_FAILED_NOTHING_TO_STEAL (0x57). Arithmetic only: the caller
/// reads the world.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wowee::game::dispel_check {

inline constexpr uint8_t kNothingToDispel = 0x56;
inline constexpr uint8_t kNothingToSteal = 0x57;

inline constexpr uint32_t kEffectDispel = 38;           // 0x26
inline constexpr uint32_t kEffectDispelMechanic = 108;  // 0x6c
inline constexpr uint32_t kEffectStealBuff = 126;       // 0x7e

[[nodiscard]] constexpr bool isRemovalEffect(uint32_t effect) {
    return effect == kEffectDispel || effect == kEffectDispelMechanic || effect == kEffectStealBuff;
}

/// 0x0076bed0: the area auras (persistent area, party, raid, pet, friend,
/// enemy, owner). An aura held from one of these is never offered.
[[nodiscard]] constexpr bool isAreaAuraEffect(uint32_t effect) {
    switch (effect) {
        case 0x1b: case 0x23: case 0x41: case 0x77: case 0x80: case 0x81: case 0x8f: return true;
        default: return false;
    }
}

/// 0x00728500's mask for a dispel type: SpellDispelType.dbc Mask (+8) where
/// the row has one, else the type's own bit.
[[nodiscard]] constexpr uint32_t dispelMask(uint32_t dispelType, uint32_t dbcMask) {
    return dbcMask != 0 ? dbcMask : (dispelType < 32 ? 1u << dispelType : 0u);
}

/// SpellDispelType.dbc's Mask (+8) for a type: the 3.3.5a table gives one
/// only to type 7 (all: magic, curse, disease and poison, 30); every other
/// type has none and stands for its own bit.
[[nodiscard]] constexpr uint32_t spellDispelTypeMask(uint32_t dispelType) { return dispelType == 7 ? 30u : 0u; }

/// Which auras of the unit count: 0x00803500 asks with -1 for AttributesEx
/// 0x10000, else 1 for a unit the caster may attack (0x00729740), 0 for one
/// it may assist (0x007293d0).
enum class Side : int8_t { Any = -1, Friendly = 0, Hostile = 1 };

/// The fields of an aura's spell 0x00728500 reads.
struct AuraSpell {
    uint32_t attributes = 0;     ///< +0x10
    uint32_t attributesEx = 0;   ///< +0x14
    uint32_t attributesEx4 = 0;  ///< +0x20
    uint32_t dispel = 0;         ///< +0x08
    uint32_t mechanic = 0;       ///< +0x0c
    std::array<uint32_t, 3> effect{};          ///< +0x11c
    std::array<uint32_t, 3> effectMechanic{};  ///< +0x14c
};

/// One aura slot of the unit: its flags byte (+0xc: bits 0-2 the effects
/// applied, 0x80 negative) and its spell, where the spell is known.
struct Aura {
    uint8_t flags = 0;
    std::optional<AuraSpell> spell;
};

/// 0x00728500: whether the unit holds an aura the removal could take.
/// `misc` is the effect's MiscValue - a dispel type, or a mechanic for a
/// mechanic dispel. `charmed` is a unit whose UNIT_FIELD_CHARMEDBY is set
/// and that is not a vehicle (guid high 0xF150): a friend under another's
/// control offers nothing, and one turned hostile only the auras that hold
/// it (effects 6, 177 or 2).
template <typename Auras>
[[nodiscard]] constexpr bool unitHasRemovable(const Auras& auras, uint32_t misc, Side side, bool mechanicDispel,
                                              bool steal, bool charmed, uint32_t dbcMask) {
    const uint32_t mask = mechanicDispel ? 0u : dispelMask(misc, dbcMask);
    bool possessedOnly = false;
    if (charmed) {
        if (side == Side::Friendly) return false;
        if (side == Side::Hostile) {
            side = Side::Friendly;
            possessedOnly = true;
        }
    }
    for (const Aura& a : auras) {
        if (side == Side::Hostile && (a.flags & 0x80u) != 0) continue;
        if (!a.spell) continue;
        const AuraSpell& s = *a.spell;
        if ((s.attributes & 0x40u) != 0) continue;  // passive
        bool area = false;
        for (int i = 0; i < 3; ++i)
            if ((a.flags & (1u << i)) != 0 && isAreaAuraEffect(s.effect[static_cast<size_t>(i)])) area = true;
        if (area) continue;
        if (possessedOnly) {
            // Only an aura applied by an effect of type 6, 177 or 2.
            bool applied = false;
            for (int i = 0; i < 3; ++i) {
                const uint32_t e = s.effect[static_cast<size_t>(i)];
                if ((e == 6 || e == 0xb1 || e == 2) && (a.flags & (1u << i)) != 0) applied = true;
            }
            if (!applied) continue;
        }
        // Channelled (AttributesEx 0x4, 0x40) or unstealable (AttributesEx4
        // 0x40) auras are not stolen.
        if (steal && ((s.attributesEx & 0x44u) != 0 || (s.attributesEx4 & 0x40u) != 0)) continue;
        if (!mechanicDispel) {
            if (s.dispel < 32 && (mask & (1u << s.dispel)) != 0) return true;
        } else {
            if (s.mechanic == misc) return true;
            for (size_t i = 0; i < 3; ++i)
                if (s.effect[i] != 0 && s.effectMechanic[i] == misc) return true;
        }
    }
    return false;
}

/// 0x0053d680, UnitAura's ninth value: whether the player's spellsteal could
/// take this aura. `stealMask` is the book's (0x00be5d68; 0 without a
/// spellsteal learned), `playerMayAttack` the player's 0x00729740 against
/// the unit. A helpful aura, not passive, not held from an area aura
/// effect, not channelled (AttributesEx 0x4, 0x40) nor unstealable
/// (AttributesEx4 0x40), whose dispel type is in the mask.
[[nodiscard]] constexpr bool auraStealable(uint32_t stealMask, bool playerMayAttack, uint8_t flags,
                                           const AuraSpell& s) {
    if (stealMask == 0 || !playerMayAttack || (flags & 0x80u) != 0) return false;
    if ((s.attributes & 0x40u) != 0) return false;
    for (int i = 0; i < 3; ++i)
        if ((flags & (1u << i)) != 0 && isAreaAuraEffect(s.effect[static_cast<size_t>(i)])) return false;
    if ((s.attributesEx & 0x44u) != 0 || (s.attributesEx4 & 0x40u) != 0) return false;
    return ((1u << (s.dispel & 31u)) & stealMask) != 0;
}

/// 0x00803500's last test over the cast's units (the caster where an
/// effect targets it, then the unit aimed at). `found(unit, effect)` says
/// whether that unit holds what that removal effect takes, or nullopt when
/// the unit is neither one to attack nor to assist and is not asked.
/// 0 lets the cast go; else the SpellCastResult it fails with.
template <typename Found>
[[nodiscard]] constexpr uint8_t refusal(const std::array<uint32_t, 3>& effects, uint32_t attributesEx4,
                                        size_t unitCount, Found found) {
    bool dispel = false;
    bool any = false;
    for (const uint32_t e : effects) {
        if (e == kEffectDispel || e == kEffectDispelMechanic) dispel = true;
        if (isRemovalEffect(e)) any = true;
    }
    if (!any) return 0;
    int removable = 0;
    bool required = false;
    for (size_t u = 0; u < unitCount; ++u) {
        for (size_t i = 0; i < 3; ++i) {
            if (!isRemovalEffect(effects[i])) continue;
            const std::optional<bool> has = found(u, i);
            if (!has) continue;
            if (*has) {
                ++removable;
            } else if ((attributesEx4 & (0x20000000u << i)) != 0) {
                required = true;
            }
        }
    }
    if (removable != 0) return 0;
    if (!required) {
        // Another effect of the spell still has work to do.
        for (const uint32_t e : effects)
            if (e != 0 && !isRemovalEffect(e)) return 0;
    }
    return dispel ? kNothingToDispel : kNothingToSteal;
}

}  // namespace wowee::game::dispel_check
