#pragma once

/// strjoin and date of the client's own library (0x00a443a8).

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstring>
#include <ctime>

#include "core/local_time.hpp"

namespace wowee::addons {

/// strjoin(separator, ...) (0x00816b60): the pieces joined by lua_concat, so one
/// that is not a string or a number raises; none is "".
inline int luaStrJoin(lua_State* L) {
    size_t len = 0;
    const char* separator = luaL_checklstring(L, 1, &len);
    const int top = lua_gettop(L);
    const int pieces = top - 1;
    if (len == 0) {
        lua_concat(L, top);
        return 1;
    }
    if (pieces == 0) {
        lua_pushstring(L, "");
        return 1;
    }
    if (!lua_checkstack(L, pieces * 2)) return luaL_error(L, "strjoin(): Stack overflow");
    for (int at = 3; at <= pieces * 2 - 1; at += 2) {
        lua_pushlstring(L, separator, len);
        lua_insert(L, at);
    }
    lua_concat(L, pieces * 2 - 1);
    return 1;
}

/// date([format[, time]]) (0x00817450): the clock here, or with a "!" before the
/// format that of Greenwich; "*t" a table of parts.
inline int luaDate(lua_State* L) {
    const char* format = luaL_optstring(L, 1, "%c");
    const std::time_t when = lua_type(L, 2) <= 0 ? std::time(nullptr)
                                                 : static_cast<std::time_t>(luaL_checknumber(L, 2));
    std::tm parts{};
    if (*format == '!') {
        parts = core::utcTime(when);
        ++format;
    } else {
        parts = core::localTime(when);
    }
    if (std::strncmp(format, "*t", 2) == 0 && format[2] == '\0') {
        lua_createtable(L, 0, 9);
        const auto set = [L](const char* key, int value) {
            lua_pushnumber(L, value);
            lua_setfield(L, -2, key);
        };
        set("sec", parts.tm_sec);
        set("min", parts.tm_min);
        set("hour", parts.tm_hour);
        set("day", parts.tm_mday);
        set("month", parts.tm_mon + 1);
        set("year", parts.tm_year + 1900);
        set("wday", parts.tm_wday + 1);
        set("yday", parts.tm_yday + 1);
        if (parts.tm_isdst >= 0) {
            lua_pushboolean(L, parts.tm_isdst);
            lua_setfield(L, -2, "isdst");
        }
        return 1;
    }
    char out[256];
    const size_t n = std::strftime(out, sizeof(out), format, &parts);
    if (n == 0) return luaL_error(L, "'date' format too long");
    lua_pushlstring(L, out, n);
    return 1;
}

}  // namespace wowee::addons
