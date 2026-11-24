extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "addons/frame_attributes.hpp"
#include "addons/lua_taint.hpp"

namespace wowee::addons {

std::string attributeKey(const char* name) {
    std::string key = name ? name : "";
    for (char& c : key)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return key;
}

void storeFrameAttribute(lua_State* L, int frame, const char* name) {
    taint::Quiet quiet;
    lua_getfield(L, frame, "__attributes");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, frame, "__attributes");
    }
    lua_insert(L, -2);
    const std::string key = attributeKey(name);
    lua_pushlstring(L, key.data(), key.size());
    lua_insert(L, -2);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

bool pushFrameAttribute(lua_State* L, int frame, const std::string& name) {
    const int caller = taint::current();
    taint::Quiet quiet;
    lua_getfield(L, frame, "__attributes");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    const std::string key = attributeKey(name.c_str());
    lua_pushlstring(L, key.data(), key.size());
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_settaintof(L, -1, caller);
    return true;
}

int getFrameAttribute(lua_State* L, const char* who) {
    const auto text = [L](int i) {
        const char* s = lua_tostring(L, i);
        return std::string(s ? s : "");
    };
    if (lua_gettop(L) == 4 && lua_isstring(L, 3)) {
        const std::string prefix = text(2), name = text(3), suffix = text(4);
        for (const std::string& n : {prefix + name + suffix, "*" + name + suffix, prefix + name + "*",
                                     "*" + name + "*", name})
            if (pushFrameAttribute(L, 1, n)) return 1;
        lua_pushnil(L);
        return 1;
    }
    if (!lua_isstring(L, 2)) return luaL_error(L, "Usage: %s:GetAttribute(\"name\")", who);
    if (!pushFrameAttribute(L, 1, text(2))) lua_pushnil(L);
    return 1;
}

void pushAttributeChange(lua_State* L, int frame, const char* name, int value) {
    lua_pushvalue(L, frame);
    lua_settaintof(L, -1, 0);
    const std::string key = attributeKey(name);
    lua_pushlstring(L, key.data(), key.size());
    lua_settaintof(L, -1, 0);
    lua_pushvalue(L, value);
    lua_settaintof(L, -1, 0);
}

}  // namespace wowee::addons
