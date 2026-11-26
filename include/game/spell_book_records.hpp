#pragma once

// The spells the client notes down as the player learns them (0x00542030),
// forgets them (0x0053fad0) and loses the whole book (0x0053ec40): the one
// spell for each job the client looks up without searching the book: the
// ranged attack, and the spells that skin, gather from or salvage a corpse.
//
// Pure, so it can be tested without a spell table: the caller hands over the
// few fields of the spell's Spell.dbc row that decide it.

#include <cstdint>

namespace wowee {
namespace game {
namespace spell_book {

/// What 0x00542030 reads of a spell's row.
struct Row {
    uint32_t attrEx4 = 0;   // AttributesEx4 (+0x20)
    uint32_t effect0 = 0;   // Effect[0] (+0x11c)
    int32_t misc0 = 0;      // EffectMiscValue[0] (+0x1b8)
    uint32_t targetA0 = 0;  // EffectImplicitTargetA[0] (+0x158)
    /// SpellDispelType.dbc's Mask (+8) of the row misc0 names, 0 where the
    /// table has no such row or the row no mask.
    uint32_t dispelMask0 = 0;
};

/// The first effect's kinds 0x00542030 notes: SPELL_EFFECT_SKINNING (95),
/// by its misc value - 0 skinning, 1 herb gathering, 2 mining, 3
/// engineering - and SPELL_EFFECT_SKIN_PLAYER_CORPSE (116).
inline constexpr uint32_t kEffectSkinning = 95;
inline constexpr uint32_t kEffectSkinPlayerCorpse = 116;

/// SPELL_EFFECT_STEAL_BENEFICIAL_BUFF (126) aimed at TARGET_UNIT_TARGET_ENEMY
/// (6) as the first effect: a spellsteal, whose dispel types make the mask
/// UnitAura's isStealable asks (0x0053d680).
inline constexpr uint32_t kEffectStealBeneficialBuff = 126;
inline constexpr uint32_t kTargetUnitTargetEnemy = 6;

/// AttributesEx4 0x01000000: the ranged attack that autoRangedCombat swaps
/// a melee swing for (Shoot, Auto Shot, Throw).
inline constexpr uint32_t kAttrEx4AutoRangedCombat = 0x01000000u;

struct Records {
    /// 0x00be5d84: the last spell learned with AttributesEx4 0x01000000.
    uint32_t rangedAttack = 0;
    /// 0x00be5d6c..0x00be5d78: the last skinning spell of each misc value.
    uint32_t skin[4] = {0, 0, 0, 0};
    /// 0x00be5d7c: the last spell that takes a player's insignia.
    uint32_t skinPlayerCorpse = 0;
    /// 0x00be5d68: the dispel types the last spellsteal learned takes - its
    /// SpellDispelType row's mask, else the type's own bit. A forgotten
    /// spell leaves it; only the book's end clears it (0x0053ec40).
    uint32_t stealMask = 0;

    /// 0x00542030: a spell learned.
    void learned(uint32_t spellId, const Row& r) {
        if (r.effect0 == kEffectStealBeneficialBuff && r.targetA0 == kTargetUnitTargetEnemy)
            stealMask = r.dispelMask0 != 0 ? r.dispelMask0 : 1u << (static_cast<uint32_t>(r.misc0) & 31u);
        if (r.effect0 == kEffectSkinning) {
            // The client indexes by the misc value unchecked; a row past
            // the four is not one it has.
            if (r.misc0 >= 0 && r.misc0 < 4) skin[r.misc0] = spellId;
        } else if (r.effect0 == kEffectSkinPlayerCorpse) {
            skinPlayerCorpse = spellId;
        }
        if ((r.attrEx4 & kAttrEx4AutoRangedCombat) != 0) rangedAttack = spellId;
    }
    /// 0x0053fad0: a spell forgotten clears whatever it was noted as; no
    /// other spell takes its place.
    void removed(uint32_t spellId) {
        if (spellId == rangedAttack) rangedAttack = 0;
        for (uint32_t& s : skin)
            if (spellId == s) s = 0;
        if (spellId == skinPlayerCorpse) skinPlayerCorpse = 0;
    }
    /// 0x0053ec40: the book emptied.
    void clear() { *this = Records{}; }
};

}  // namespace spell_book
}  // namespace game
}  // namespace wowee
