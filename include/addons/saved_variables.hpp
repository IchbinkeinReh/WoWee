#pragma once

// The text of a SavedVariables file, as the client writes it (0x00818b50, 0x008183a0).

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::addons::saved_variables {

/// Whether a variable of that name is written (0x005f5620): letters, digits, underscores and
/// what is past ASCII.
[[nodiscard]] inline bool nameWritten(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x80 && !(u >= '0' && u <= '9') && !(u >= 'a' && u <= 'z') && !(u >= 'A' && u <= 'Z') && u != '_')
            return false;
    }
    return true;
}

namespace detail {
inline constexpr size_t kDeepest = 150;

inline void appendString(lua_State* L, int idx, std::string& out) {
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n);
    out += '"';
    for (size_t i = 0; i < n; ++i) {
        switch (s[i]) {
            case '\0': out += "\\000"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '"':
            case '\\': out += '\\'; [[fallthrough]];
            default: out += s[i];
        }
    }
    out += '"';
}

inline void appendValue(lua_State* L, int idx, std::string& out, std::vector<const void*>& open);

inline void appendTable(lua_State* L, int idx, std::string& out, std::vector<const void*>& open) {
    const void* table = lua_topointer(L, idx);
    for (const void* t : open)
        if (t == table) {
            out += "nil --[[ skipped recursive table ]]";
            return;
        }
    // Lua reads no more than 200 levels (LUAI_MAXCCALLS): deeper, the whole file would be lost.
    if (open.size() >= kDeepest || !lua_checkstack(L, 4)) {
        out += "nil --[[ skipped table ]]";
        return;
    }
    open.push_back(table);
    out += "{\r\n";
    const std::string tabs(open.size(), '\t');
    const int count = static_cast<int>(lua_objlen(L, idx));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, idx, i);
        out += tabs;
        appendValue(L, lua_gettop(L), out, open);
        out += ", -- [" + std::to_string(i) + "]\r\n";
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        const int value = lua_gettop(L), key = value - 1;
        const int type = lua_type(L, key);
        const double number = type == LUA_TNUMBER ? lua_tonumber(L, key) : 0.0;
        if (type == LUA_TNUMBER && std::isnan(number)) {
            out += tabs + "--[[ skipped entry with nan key ]]\r\n";
        } else if (type == LUA_TNUMBER && std::isinf(number)) {
            out += tabs + "--[[ skipped entry with inf key ]]\r\n";
        } else if (type == LUA_TNUMBER && number >= 1.0 && number <= count && number == std::floor(number)) {
            // Above already.
        } else if (type == LUA_TNUMBER || type == LUA_TSTRING || type == LUA_TBOOLEAN) {
            out += tabs + "[";
            appendValue(L, key, out, open);
            out += "] = ";
            appendValue(L, value, out, open);
            out += ",\r\n";
        } else {
            out += tabs + "--[[ skipped entry with " + lua_typename(L, type) + " key ]]\r\n";
        }
        lua_pop(L, 1);
    }
    open.pop_back();
    out += std::string(open.size(), '\t') + "}";
}

inline void appendValue(lua_State* L, int idx, std::string& out, std::vector<const void*>& open) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL: out += "nil"; break;
        case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; break;
        case LUA_TNUMBER: {
            const double v = lua_tonumber(L, idx);
            if (std::isnan(v)) {
                out += "nil --[[ nan ]]";
            } else if (std::isinf(v)) {
                out += "nil --[[ inf ]]";
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.16g", v);
                out += buf;
            }
            break;
        }
        case LUA_TSTRING: appendString(L, idx, out); break;
        case LUA_TTABLE: appendTable(L, idx, out, open); break;
        case LUA_TFUNCTION: out += "nil --[[ skipped inline function ]]"; break;
        default: out += std::string("nil --[[ skipped ") + lua_typename(L, lua_type(L, idx)) + " ]]"; break;
    }
}
}  // namespace detail

/// The value at `idx` appended: the list part of a table first with its numbers behind, then
/// the rest; what cannot be read back as nil with what it was, a table within itself too.
inline void appendValue(lua_State* L, int idx, std::string& out) {
    std::vector<const void*> open;
    detail::appendValue(L, idx < 0 && idx > LUA_REGISTRYINDEX ? lua_gettop(L) + idx + 1 : idx, out, open);
}

/// The file of these globals, each as it is, nil too. Raw, so that what stands in for a
/// missing function is no value.
[[nodiscard]] inline std::string fileText(lua_State* L, const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!nameWritten(name)) continue;
        out += "\r\n" + name + " = ";
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_pushlstring(L, name.data(), name.size());
        lua_rawget(L, -2);
        appendValue(L, -1, out);
        lua_pop(L, 2);
    }
    return out + "\r\n";
}

}  // namespace wowee::addons::saved_variables
