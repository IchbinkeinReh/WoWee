#pragma once

/**
 * unit_aura_call.hpp - the UnitAura call a tooltip's SetUnitAura makes.
 *
 * GameTooltip:SetUnitAura(unit, index [, filter]) and
 * GameTooltip:SetUnitAura(unit, name, rank [, filter]) read their aura with
 * the parser UnitAura itself uses (0x00626240 -> 0x006110a0 -> 0x00608390),
 * so the binding here asks UnitAura with its own arguments, as given.
 *
 * Those are copied from fixed stack slots, and a slot the caller left out has
 * to read as nil. It did not: the copies were made after UnitAura had been
 * pushed, so with no filter the fourth slot *was* UnitAura, the function went
 * to UnitAura as its filter, UnitAura raised "string expected, got function"
 * inside the pcall, and SetUnitAura("player", 1) built nothing - while
 * SetUnitBuff, which always hands a filter on, worked.
 */

extern "C" {
#include "lua.h"
}

namespace wowee::addons {

/// The aura UnitAura last answered with: SetUnitAura's builder (0x00625350)
/// reads the aura's own caster level and count off it for the description,
/// neither of which UnitAura returns. Cleared before the setter's call, so a
/// hooked UnitAura that answers otherwise leaves it invalid.
struct UnitAuraAnswer {
    bool valid = false;
    int level = 0;
    int count = 0;
};
UnitAuraAnswer& lastUnitAuraAnswer();

/// UnitAura(unit, index or name, filter or rank, filter) from a setter's
/// (self, unit, ...) arguments. True with UnitAura's eleven answers on top of
/// the stack; false with nothing pushed, when UnitAura is missing or raised.
///
/// The stack is first cut to the five slots the call reads, so an argument
/// left out is nil wherever it is read from.
inline bool callUnitAuraForSetter(lua_State* L) {
    lua_settop(L, 5);
    lua_getglobal(L, "UnitAura");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    for (int i = 2; i <= 5; ++i) lua_pushvalue(L, i);
    if (lua_pcall(L, 4, 11, 0) != 0) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

}  // namespace wowee::addons
