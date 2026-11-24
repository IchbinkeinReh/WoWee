#include "addons/lua_std_libs.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdint>
#include <initializer_list>

namespace wowee::addons {

namespace {

// The low 32 bits of the number as a whole one, as _ftol2 (0x0088b9f6) has
// them; a number past 64 bits has none.
uint32_t bitsOf(lua_State* L, int arg) {
    const double v = luaL_checknumber(L, arg);
    if (!(v > -9.2e18 && v < 9.2e18)) return 0;
    return static_cast<uint32_t>(static_cast<int64_t>(v));
}

int pushBits(lua_State* L, uint32_t v) {
    lua_pushnumber(L, static_cast<double>(v));
    return 1;
}

int bitNot(lua_State* L) { return pushBits(L, ~bitsOf(L, 1)); }   // 0x00850cc0

// 0x00850d00 and the two after it: as many as there are.
int bitAnd(lua_State* L) {
    uint32_t v = bitsOf(L, 1);
    for (int i = 2; i <= lua_gettop(L); ++i) v &= bitsOf(L, i);
    return pushBits(L, v);
}
int bitOr(lua_State* L) {
    uint32_t v = bitsOf(L, 1);
    for (int i = 2; i <= lua_gettop(L); ++i) v |= bitsOf(L, i);
    return pushBits(L, v);
}
int bitXor(lua_State* L) {
    uint32_t v = bitsOf(L, 1);
    for (int i = 2; i <= lua_gettop(L); ++i) v ^= bitsOf(L, i);
    return pushBits(L, v);
}

// Shifts count modulo 32 (0x00850e80, 0x00850ee0, 0x00850f40).
int bitLeft(lua_State* L) {
    const uint32_t by = bitsOf(L, 2) & 31u;
    return pushBits(L, bitsOf(L, 1) << by);
}
int bitRight(lua_State* L) {
    const uint32_t by = bitsOf(L, 2) & 31u;
    return pushBits(L, bitsOf(L, 1) >> by);
}
int bitArithmeticRight(lua_State* L) {
    const uint32_t by = bitsOf(L, 2) & 31u;
    lua_pushnumber(L, static_cast<double>(static_cast<int32_t>(bitsOf(L, 1)) >> by));
    return 1;
}

// 0x00850f90: the rest of two whole numbers, with the sign of the first; by
// one that is none, 1 over it.
int bitMod(lua_State* L) {
    const auto by = static_cast<int32_t>(bitsOf(L, 2));
    if (by == 0) {
        lua_pushnumber(L, 1.0 / lua_tonumber(L, 2));
        return 1;
    }
    const auto v = static_cast<int32_t>(bitsOf(L, 1));
    lua_pushnumber(L, by == -1 ? 0.0 : static_cast<double>(v % by));
    return 1;
}

// 0x00851b20: count values of a list from a place on, answered in order, the
// rest moved down.
int tableRemoveMulti(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const int size = static_cast<int>(lua_objlen(L, 1));
    const int at = static_cast<int>(luaL_optinteger(L, 2, size));
    const int count = static_cast<int>(luaL_optinteger(L, 3, 1));
    if (size == 0) return 0;
    if (at < 1 || count < 0 || at > size || static_cast<int64_t>(at) + count - 1 > size)
        return luaL_error(L, "parameters out of bounds");
    luaL_checkstack(L, count, "too many values");
    for (int i = at; i < at + count; ++i) lua_rawgeti(L, 1, i);
    for (int i = at; i + count <= size; ++i) {
        lua_rawgeti(L, 1, i + count);
        lua_rawseti(L, 1, i);
    }
    for (int i = size - count + 1; i <= size; ++i) {
        lua_pushnil(L);
        lua_rawseti(L, 1, i);
    }
    return count;
}

const luaL_Reg kBit[] = {
    {"bnot", bitNot},       {"band", bitAnd},       {"bor", bitOr},
    {"bxor", bitXor},       {"lshift", bitLeft},    {"rshift", bitRight},
    {"arshift", bitArithmeticRight}, {"mod", bitMod}, {nullptr, nullptr},
};

void dropField(lua_State* L, const char* table, const char* name) {
    lua_getglobal(L, table);
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void alias(lua_State* L, const char* global, const char* table, const char* field) {
    lua_getglobal(L, table);
    lua_getglobal(L, global);
    if (!lua_isnil(L, -1)) lua_setfield(L, -2, field);
    else lua_pop(L, 1);
    lua_pop(L, 1);
}

}  // namespace

void openInterfaceLibs(lua_State* L) {
    for (const lua_CFunction open : {luaopen_base, luaopen_table, luaopen_string, luaopen_math}) {
        lua_pushcfunction(L, open);
        lua_call(L, 0, 0);
    }
    // newproxy stays, despite the name. It returns a userdata with a fresh
    // metatable and reaches nothing else; what it buys is __index and
    // __newindex on a value that cannot be tampered with, which is exactly how
    // Blizzard's own RestrictedFrames builds secure frame handles.
    for (const char* name : {"dofile", "loadfile", "load"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    dropField(L, "math", "randomseed");

    lua_getglobal(L, "table");
    lua_pushcfunction(L, tableRemoveMulti);
    lua_setfield(L, -2, "removemulti");
    lua_pop(L, 1);

    luaL_register(L, "bit", kBit);
    lua_pop(L, 1);
}

void setInterfaceLibAliases(lua_State* L) {
    lua_getglobal(L, "table");
    for (const char* name : {"foreach", "foreachi"}) {
        lua_getfield(L, -1, name);
        lua_setglobal(L, name);
    }
    lua_pop(L, 1);
    alias(L, "strtrim", "string", "trim");
    alias(L, "strsplit", "string", "split");
    alias(L, "strjoin", "string", "join");
    alias(L, "strreplace", "string", "replace");
}

void dropForeignGlobals(lua_State* L) {
    static constexpr const char* kForeign[] = {
        // 1.12's and 2.4's: buffs by slot, and a name spelled its own way.
        "GetPlayerBuff", "GetPlayerBuffApplications", "GetPlayerBuffDispelType", "GetPlayerBuffTexture",
        "GetPlayerBuffTimeLeft", "CancelPlayerBuff", "GetNumLaguages", "GetInspectPVPRankProgress",
        // FrameXML names these where it never comes by (moneyframe.lua,
        // blizzard_macroui.lua); the client has none of them.
        "AddSendMailMoney", "AddSendMailCOD", "PickupSendMailMoney", "PickupSendMailCOD", "SaveMacros",
        // Later clients'.
        "GetSpellBookItemInfo", "GetSpellBookItemName", "PickupSpellBookItem", "IsSpellPassive", "GetNumBagSlots",
    };
    for (const char* name : kForeign) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
}

}  // namespace wowee::addons
