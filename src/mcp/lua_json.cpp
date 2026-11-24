#include "mcp/lua_json.hpp"

#include <cmath>
#include <cstdio>

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/application.hpp"
#include "mcp/tool_context.hpp"
#include "ui/plural_escape.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace wowee::mcp {

namespace {

/// Nothing a tool reports is nested deeper.
constexpr int kMostDepth = 8;
constexpr int kMostValues = 4000;

/// 1 to n with no holes and nothing else in it; n, or -1.
int arrayLength(lua_State* L, int index) {
    const int n = static_cast<int>(lua_objlen(L, index));
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        lua_pop(L, 1);
        ++count;
        if (count > n || lua_type(L, -1) != LUA_TNUMBER) {
            lua_pop(L, 1);
            return -1;
        }
    }
    return count == n ? n : -1;
}

}  // namespace

json luaToJson(lua_State* L, int index, int depth, int& budget) {
    if (index < 0) index = lua_gettop(L) + index + 1;
    if (--budget < 0) return nullptr;
    switch (lua_type(L, index)) {
        case LUA_TNIL:
        case LUA_TNONE:
            return nullptr;
        case LUA_TBOOLEAN:
            return lua_toboolean(L, index) != 0;
        case LUA_TNUMBER: {
            const double v = lua_tonumber(L, index);
            if (!std::isfinite(v)) return nullptr;
            if (std::floor(v) == v && std::fabs(v) < 9.0e15) return static_cast<long long>(v);
            // Three places are more than anything here is measured to.
            return std::round(v * 1000.0) / 1000.0;
        }
        case LUA_TSTRING: {
            size_t length = 0;
            const char* s = lua_tolstring(L, index, &length);
            return std::string(s, length);
        }
        case LUA_TTABLE: {
            if (depth >= kMostDepth || !lua_checkstack(L, 8)) return "...";
            const int n = arrayLength(L, index);
            // One with nothing in it is a list of nothing: what these chunks
            // leave empty are lists.
            if (n >= 0) {
                json out = json::array();
                for (int i = 1; i <= n && budget > 0; ++i) {
                    lua_rawgeti(L, index, i);
                    out.push_back(luaToJson(L, lua_gettop(L), depth + 1, budget));
                    lua_pop(L, 1);
                }
                return out;
            }
            json out = json::object();
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    size_t length = 0;
                    const char* s = lua_tolstring(L, -2, &length);
                    key.assign(s, length);
                } else if (lua_type(L, -2) == LUA_TNUMBER) {
                    char number[32];
                    std::snprintf(number, sizeof(number), "%.14g", lua_tonumber(L, -2));
                    key = number;
                }
                if (!key.empty() && budget > 0) out[key] = luaToJson(L, lua_gettop(L), depth + 1, budget);
                lua_pop(L, 1);
            }
            return out;
        }
        default:
            return std::string(lua_typename(L, lua_type(L, index)));
    }
}

json queryLua(ToolContext& context, const std::string& chunk) {
    auto* manager = context.app.getAddonManager();
    auto* engine = manager ? manager->getLuaEngine() : nullptr;
    if (!engine || !engine->isInitialized()) throw ToolError("the interface is not loaded");
    json out;
    std::string error;
    const bool ok = engine->query(
        chunk,
        [&out](lua_State* L, int index) {
            int budget = kMostValues;
            out = luaToJson(L, index, 0, budget);
        },
        error);
    if (!ok) throw ToolError("Lua: " + error);
    return out;
}

json tryQueryLua(ToolContext& context, const std::string& chunk) {
    try {
        return queryLua(context, chunk);
    } catch (const ToolError&) {
        return nullptr;
    }
}

std::string luaQuote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char code[8];
            std::snprintf(code, sizeof(code), "\\%03u", u);
            out += code;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string plainText(const std::string& text) {
    // "15 |4Min.:Min.;" as a font string has it, by the count before it.
    const std::string marked = ui::resolvePluralEscapes(text);
    std::string out;
    for (size_t i = 0; i < marked.size(); ++i) {
        if (marked[i] != '|' || i + 1 >= marked.size()) {
            out += marked[i];
            continue;
        }
        const char what = marked[i + 1];
        if (what == '|') {
            out += '|';
            ++i;
        } else if (what == 'c' && i + 9 < marked.size()) {
            i += 9;  // |cAARRGGBB
        } else if (what == 'r' || what == 'h') {
            ++i;
        } else if (what == 'H') {
            // |Hpayload|h[text]|h: the text.
            const size_t end = marked.find("|h", i + 2);
            if (end == std::string::npos) return out;
            i = end + 1;
        } else if (what == 'T') {
            const size_t end = marked.find("|t", i + 2);
            if (end == std::string::npos) return out;
            i = end + 1;
        } else if (what == 'n') {
            out += '\n';
            ++i;
        } else {
            out += marked[i];
        }
    }
    return out;
}

}  // namespace wowee::mcp
