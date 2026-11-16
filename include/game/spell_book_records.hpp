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
};

/// The first effect's kinds 0x00542030 notes: SPELL_EFFECT_SKINNING (95),
/// by its misc value - 0 skinning, 1 herb gathering, 2 mining, 3
/// engineering - and SPELL_EFFECT_SKIN_PLAYER_CORPSE (116).
inline constexpr uint32_t kEffectSkinning = 95;
inline constexpr uint32_t kEffectSkinPlayerCorpse = 116;

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

    /// 0x00542030: a spell learned.
    void learned(uint32_t spellId, const Row& r) {
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
