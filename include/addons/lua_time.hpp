#pragma once

/// time() and its date table (0x00817650), which the client has from os.time.

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <ctime>

namespace wowee::addons {

inline int luaTimeOf(lua_State* L) {
    std::time_t t = -1;
    if (lua_isnoneornil(L, 1)) {
        t = std::time(nullptr);
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 1);
        const auto field = [L](const char* key, int missing) {
            lua_getfield(L, 1, key);
            int value = missing;
            if (lua_isnumber(L, -1)) value = static_cast<int>(lua_tointeger(L, -1));
            else if (missing < 0) luaL_error(L, "field '%s' missing in date table", key);
            lua_pop(L, 1);
            return value;
        };
        std::tm tm{};
        tm.tm_sec = field("sec", 0);
        tm.tm_min = field("min", 0);
        tm.tm_hour = field("hour", 12);
        tm.tm_mday = field("day", -1);
        tm.tm_mon = field("month", -1) - 1;
        tm.tm_year = field("year", -1) - 1900;
        lua_getfield(L, 1, "isdst");
        tm.tm_isdst = lua_isnil(L, -1) ? -1 : lua_toboolean(L, -1);
        lua_pop(L, 1);
        t = std::mktime(&tm);
    }
    if (t == static_cast<std::time_t>(-1)) lua_pushnil(L);
    else lua_pushnumber(L, static_cast<lua_Number>(t));
    return 1;
}

}  // namespace wowee::addons
