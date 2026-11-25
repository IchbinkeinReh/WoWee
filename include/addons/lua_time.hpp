#pragma once

/// time() and its date table (0x00817650), which the client has from os.time.

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <ctime>

namespace wowee::addons {

/// mktime as the client's is (__mktime32, 0x0088e157): -1 for a year outside 1969 to 2039, its
/// months carried into it, and for what does not fit 31 bits. The platforms' own are of 64 bits,
/// and take what it does not.
inline std::time_t clientMktime(std::tm tm) {
    constexpr int kFirst = 69, kLast = 139;
    if (tm.tm_year < kFirst || tm.tm_year > kLast) return -1;
    int year = tm.tm_year + tm.tm_mon / 12;
    if (tm.tm_mon % 12 < 0) --year;
    if (year < kFirst || year > kLast) return -1;
    const std::time_t t = std::mktime(&tm);
    return t < 0 || t > static_cast<std::time_t>(0x7fffffff) ? static_cast<std::time_t>(-1) : t;
}

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
        t = clientMktime(tm);
    }
    if (t == static_cast<std::time_t>(-1)) lua_pushnil(L);
    else lua_pushnumber(L, static_cast<lua_Number>(t));
    return 1;
}

}  // namespace wowee::addons
