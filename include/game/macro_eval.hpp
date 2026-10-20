#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee::game::macro_eval {

/// What a macro shows: the spell or item its icon, tooltip and cooldown follow,
/// as the client works it out from the macro's text (UIMacros.cpp).
///
/// The text is read once, when the macro is made or edited (0x00565c40): the
/// lines that name something - #show / #showtooltip with an argument, and the
/// casting slash commands - become a short list of commands. A command with a
/// conditional or a slot number, or a /castsequence, can answer differently
/// from one moment to the next, which makes the macro "dynamic", and reading
/// goes on to the next line; the first command that cannot change, and any
/// #show, ends it. Which of the commands applies, and what it names, is then
/// worked out (0x005650c0) - again whenever something a dynamic macro depends
/// on changes.

/// The kind of a command (the slash command table at 0x00ace300).
enum Kind : int {
    kShow = 1,        ///< #show, #showtooltip
    kCast = 2,        ///< SLASH_CAST, SLASH_USE
    kRandom = 3,      ///< SLASH_CASTRANDOM, SLASH_USERANDOM: the first of the list
    kSequence = 4,    ///< SLASH_CASTSEQUENCE: QueryCastSequence's current entry
    kEquip = 5,       ///< SLASH_EQUIP: an item only
    kEquipSlot = 6,   ///< SLASH_EQUIP_TO_SLOT: a slot number, then an item
};

struct Command {
    int kind = 0;
    std::string text;   ///< everything after the command word, trimmed of spaces
};

struct Parsed {
    std::vector<Command> commands;
    bool showTooltip = false;   ///< +0x570: a #showtooltip line
    bool dynamic = false;       ///< +0x574
};

/// 0x00565c40. `slashKind` names the kind of a slash command word ("/cast"),
/// 0 for one that is not a casting command.
Parsed parse(const std::string& body, const std::function<int(const std::string& word)>& slashKind);

/// What evaluation needs from the client.
struct Hooks {
    /// The conditional parser (0x005f0df0) on a command that opens with '[':
    /// false when no clause applies; else the applying clause's text and its
    /// target unit (0 for none).
    std::function<bool(const std::string& text, std::string& result, uint64_t& target)> options;
    /// QueryCastSequence(sequence): the entry the sequence is on - its item if
    /// it has one, else its spell; empty for none.
    std::function<std::string(const std::string& sequence)> castSequence;
    /// 0x00709de0: an "item:<id>" link or an item's exact name, 0 for none.
    std::function<uint32_t(const std::string& name)> itemByName;
    /// 0x00540200: a spell in the player's or pet's spellbook by name, with an
    /// optional "(rank)"; 0 for none. `pet` says which book it came from.
    std::function<uint32_t(const std::string& name, bool& pet)> spellByName;
    /// The item in an equipment or backpack slot (bag -1, slot 0..38) or in a
    /// bag's slot (bag 0..3), 0 for none.
    std::function<uint32_t(int bag, int slot)> itemInSlot;
};

struct Result {
    int32_t spell = 0;    ///< +0x564: 0 no command applied, -1 one applied naming nothing
    uint32_t item = 0;    ///< +0x568
    bool petSpell = false;///< +0x56c
    uint64_t target = 0;  ///< +0x578
};

/// 0x005650c0: what the first applying command names.
Result evaluate(const Parsed& parsed, const Hooks& hooks);

}  // namespace wowee::game::macro_eval
