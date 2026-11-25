extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "addons/frame_handle.hpp"
#include "addons/lua_taint.hpp"

namespace wowee::addons {

namespace {

// A widget's id, marked: what else a table may have at 0 is no handle.
constexpr uintptr_t kMark = 0x9;

// The tables by their widgets.
constexpr const char* kObjects = "wowee_frame_handles";

int methodByHandle(lua_State* L) {
    takeSelfByHandle(L);
    return lua_tocfunction(L, lua_upvalueindex(1))(L);
}

int scriptMethodByHandle(lua_State* L) {
    takeSelfByHandle(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

}  // namespace

void setFrameHandle(lua_State* L, uint32_t widget) {
    taint::Quiet quiet;
    lua_pushlightuserdata(L, reinterpret_cast<void*>((static_cast<uintptr_t>(widget) << 4) | kMark));
    lua_rawseti(L, -2, 0);
    lua_getfield(L, LUA_REGISTRYINDEX, kObjects);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kObjects);
    }
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, static_cast<int>(widget));
    lua_pop(L, 1);
}

uint32_t widgetOfHandle(const void* handle) {
    const auto value = reinterpret_cast<uintptr_t>(handle);
    return (value & 0xF) == kMark ? static_cast<uint32_t>(value >> 4) : 0;
}

void pushFrameByHandle(lua_State* L, uint32_t widget) {
    const int caller = taint::current();
    taint::Quiet quiet;
    lua_getfield(L, LUA_REGISTRYINDEX, kObjects);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, static_cast<int>(widget));
        lua_remove(L, -2);
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    lua_settaintof(L, -1, caller);
}

int pushObjects(lua_State* L, const std::vector<uint32_t>& widgets) {
    if (!lua_checkstack(L, static_cast<int>(widgets.size()) + 4)) return -1;
    int pushed = 0;
    for (const uint32_t widget : widgets) {
        pushFrameByHandle(L, widget);
        if (lua_istable(L, -1)) ++pushed;
        else lua_pop(L, 1);
    }
    return pushed;
}

void takeSelfByHandle(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) return;
    const int caller = taint::current();
    taint::Quiet quiet;
    // A frame's own table knows its widget.
    lua_pushlstring(L, "__wid", 5);
    lua_rawget(L, 1);
    const bool itself = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (itself) return;
    lua_rawgeti(L, 1, 0);
    const uint32_t widget = lua_islightuserdata(L, -1) ? widgetOfHandle(lua_touserdata(L, -1)) : 0;
    lua_pop(L, 1);
    if (widget == 0) return;
    pushFrameByHandle(L, widget);
    if (lua_istable(L, -1)) {
        lua_settaintof(L, -1, caller);
        lua_replace(L, 1);
    } else {
        lua_pop(L, 1);
    }
}

void pushMethodByHandle(lua_State* L, lua_CFunction method) {
    lua_pushcfunction(L, method);
    lua_pushcclosure(L, methodByHandle, 1);
}

void makeMethodByHandle(lua_State* L) {
    const lua_CFunction method = lua_tocfunction(L, -1);
    lua_pushcclosure(L, method ? methodByHandle : scriptMethodByHandle, 1);
}

void makeMethodsByHandle(lua_State* L) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || !lua_isfunction(L, -1) || lua_iscfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        makeMethodByHandle(L);
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, -4);
    }
}

}  // namespace wowee::addons
