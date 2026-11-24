#pragma once

/// What the cursor holds and an action bar slot: whether a click puts it
/// there (UseAction, 0x005abbc0; PickupAction, 0x005abe70), what the slot
/// takes (0x005ab120), and what a click into the world does with it.

#include "game/totem_bar.hpp"

#include <cstdint>

namespace wowee::game::action_drop {

/// The held kinds an action bar takes: a spell (0x005136c0), an item by
/// itself or by its entry (0x00513660, 0x005844a0(7)), a macro (0x00bd07e8)
/// and an equipment set (0x00ac80a0). Money, a vendor's item, ammunition, a
/// guild bank item, a pet's action and a stabled pet are none of them.
enum class Kind : uint8_t { None, Spell, Item, Macro, EquipmentSet };

struct Action {
    Kind kind = Kind::None;
    uint32_t id = 0;
};

/// The 144 action slots, 0-based, but the bonus bar's (0x78..0x83), which are
/// the pet's or the vehicle's own.
[[nodiscard]] constexpr bool slotTakesDrops(int slot) {
    return slot >= 0 && slot < 0x90 && (slot < 0x78 || slot > 0x83);
}

/// UseAction puts what is held into the slot instead of using it - by a
/// click, not while a key binding runs (0x00beadd8 + 0x12c, set by
/// 0x00563150 around every binding).
[[nodiscard]] constexpr bool useActionPlaces(int slot, const Action& held, bool bindingRunning) {
    return !bindingRunning && slotTakesDrops(slot) && held.kind != Kind::None;
}

struct Facts {
    bool spellKnown = false;                     ///< the held spell has a Spell.dbc row
    bool spellPassive = false;                   ///< its Attributes 0x40
    uint32_t spellTotemCategories[2] = {0, 0};   ///< its RequiredTotemCategoryID
    bool itemKnown = false;                      ///< the held item has an Item.dbc row
    bool itemUseSpell = false;                   ///< one of its spells is used (0x00706b90)
    uint32_t itemInventoryType = 0;
    bool slotActionThere = true;   ///< the slot's macro or set still exists
};

enum class Result : uint8_t {
    Keep,          ///< nothing happens
    PutDown,       ///< the slot's own action again: the cursor lets it go, the slot keeps it
    PassiveSpell,  ///< ERR_PASSIVE_ABILITY, nothing else happens
    WrongTotem,    ///< ERR_MULTI_CAST_ACTION_TOTEM_S, nothing else happens
    Place,         ///< into the slot; what it held comes onto the cursor
};

/// 0x005ab120.
[[nodiscard]] constexpr Result drop(int slot, const Action& held, const Action& inSlot, const Facts& f) {
    if (slot < 0 || slot >= 0x90 || held.kind == Kind::None) return Result::Keep;
    if (held.kind == Kind::Spell && !f.spellKnown) return Result::Keep;
    if (slot > 0x83 && (held.kind != Kind::Spell || !totem_bar::slotTakes(slot, f.spellTotemCategories)))
        return Result::WrongTotem;
    if (held.kind == Kind::Spell && f.spellPassive) return Result::PassiveSpell;
    // An item with something to use, or one that is worn but no bag.
    if (held.kind == Kind::Item &&
        (!f.itemKnown || (!f.itemUseSpell && (f.itemInventoryType == 0 || f.itemInventoryType == 18))))
        return Result::Keep;
    if (inSlot.kind == held.kind && inSlot.id == held.id && f.slotActionThere) return Result::PutDown;
    return Result::Place;
}

/// What the cursor holds, as the world's clicks ask it.
struct Held {
    bool any = false;       ///< 0x00bd0748 is not 0
    bool ownItem = false;   ///< an item lifted out of a bag or off the character (0x00bd0768, 0x00bd0760)
    bool byEntry = false;   ///< an item known by its entry alone (0x00bd0758)
    bool equipmentSet = false;
};

enum class WorldClick : uint8_t {
    Through,        ///< the click does what it does with nothing held
    PutDown,        ///< the cursor lets it go, and that is all
    PutDownFirst,   ///< the cursor lets it go, and the click goes on
    ConfirmDelete,  ///< DELETE_ITEM_CONFIRM, and that is all
};

/// A left click: on no object, 0x005278c0 - the ground is none while the
/// cursor holds something (0x004f7650) - and on one, 0x00527870.
[[nodiscard]] constexpr WorldClick leftClick(const Held& h, bool onObject) {
    if (onObject) return h.byEntry ? WorldClick::PutDownFirst : WorldClick::Through;
    if (!h.any) return WorldClick::Through;
    return h.ownItem ? WorldClick::ConfirmDelete : WorldClick::PutDown;
}

/// The right button pressed, anywhere: the cursor lets go of what it holds
/// and the press goes no further (0x0051fa50) - but for an equipment set,
/// which is not asked about. `wrapping`: a gift wrap waits for its item
/// (0x006cefb0).
[[nodiscard]] constexpr bool rightPressPutsDown(const Held& h, bool wrapping) {
    return wrapping || (h.any && !h.equipmentSet);
}

}  // namespace wowee::game::action_drop
