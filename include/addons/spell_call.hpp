#pragma once

namespace wowee::addons {

/// How the arguments of a spell function name a spell (0x00540670).
enum class SpellCall {
    Id,        ///< GetSpellInfo(133): a spell's number, alone
    Slot,      ///< (13, "spell"): a place in the player's book or the pet's
    NameRank,  ///< ("Schattenblitz", "Rang 3")
    Name,      ///< ("Schattenblitz"), or with a unit after it
    None,      ///< "<function>(): Invalid spell slot"
};

/// `number1`, `text1` and `text2`: the first argument is a number, and a text
/// (which a number is, too); the second one is. `secondIsUnit`: a unit's token
/// or a name of the group (0x0060abf0). GetSpellInfo, GetSpellLink,
/// GetSpellCooldown and IsCurrentSpell take a number alone for a spell's
/// (`takesId`); CastSpell takes no name.
[[nodiscard]] constexpr SpellCall spellCallOf(int count, bool number1, bool text1, bool text2, bool secondIsUnit,
                                              bool takesId, bool takesName) {
    if (takesId && number1 && count == 1) return SpellCall::Id;
    if (number1 && text2) return SpellCall::Slot;
    if (!takesName || !text1) return SpellCall::None;
    return text2 && !secondIsUnit ? SpellCall::NameRank : SpellCall::Name;
}

}  // namespace wowee::addons
