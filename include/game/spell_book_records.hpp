#pragma once

// The spells the client notes down as the player learns them (0x00542030),
// forgets them (0x0053fad0) and loses the whole book (0x0053ec40): the one
// spell for each job the client looks up without searching the book.
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
};

/// AttributesEx4 0x01000000: the ranged attack that autoRangedCombat swaps
/// a melee swing for (Shoot, Auto Shot, Throw).
inline constexpr uint32_t kAttrEx4AutoRangedCombat = 0x01000000u;

struct Records {
    /// 0x00be5d84: the last spell learned with AttributesEx4 0x01000000.
    uint32_t rangedAttack = 0;

    /// 0x00542030: a spell learned.
    void learned(uint32_t spellId, const Row& r) {
        if ((r.attrEx4 & kAttrEx4AutoRangedCombat) != 0) rangedAttack = spellId;
    }
    /// 0x0053fad0: a spell forgotten clears whatever it was noted as; no
    /// other spell takes its place.
    void removed(uint32_t spellId) {
        if (spellId == rangedAttack) rangedAttack = 0;
    }
    /// 0x0053ec40: the book emptied.
    void clear() { *this = Records{}; }
};

}  // namespace spell_book
}  // namespace game
}  // namespace wowee
