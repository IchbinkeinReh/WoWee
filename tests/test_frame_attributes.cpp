// A frame's attributes as the client keeps them (0x0049f2d0, 0x0049f610).
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "addons/frame_attributes.hpp"
#include "addons/lua_std_libs.hpp"
#include "addons/lua_taint.hpp"

#include <string>

namespace {

namespace taint = wowee::addons::taint;
using namespace wowee::addons;

int get(lua_State* L) { return getFrameAttribute(L, "F"); }
int set(lua_State* L) {
    lua_settop(L, 3);
    storeFrameAttribute(L, 1, lua_tostring(L, 2));
    return 0;
}

struct State {
    lua_State* L = luaL_newstate();
    State() {
        openInterfaceLibs(L);
        luaL_dostring(L, "F = {}");
        lua_pushcfunction(L, get);
        lua_setglobal(L, "Get");
        lua_pushcfunction(L, set);
        lua_setglobal(L, "Set");
    }
    ~State() { lua_close(L); }
    std::string run(const std::string& chunk) {
        if (luaL_dostring(L, chunk.c_str()) != 0) {
            const std::string err = lua_tostring(L, -1);
            lua_settop(L, 0);
            return "error: " + err;
        }
        std::string out;
        const int n = lua_gettop(L);
        for (int i = 1; i <= n; ++i) {
            lua_getglobal(L, "tostring");
            lua_pushvalue(L, i);
            lua_call(L, 1, 1);
            out += (out.empty() ? "" : " ") + std::string(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_settop(L, 0);
        return out;
    }
};

}  // namespace

TEST_CASE("attributes by their names in any case", "[lua]") {
    State s;
    s.run("Set(F, 'FooBar', 7)");
    CHECK(s.run("return Get(F, 'foobar'), Get(F, 'FOOBAR'), Get(F, 'FooBar')") == "7 7 7");
    s.run("Set(F, 'fooBAR', false)");
    CHECK(s.run("return Get(F, 'FooBar')") == "false");
    s.run("Set(F, 'foobar', nil)");
    CHECK(s.run("return Get(F, 'FooBar')") == "nil");
    CHECK(s.run("return Get({}, 'x')") == "nil");
    CHECK(s.run("Get(F)").find("Usage: F:GetAttribute(\"name\")") != std::string::npos);
    CHECK(attributeKey("_OnState-X") == "_onstate-x");
}

TEST_CASE("an attribute in pieces, the nearest first", "[lua]") {
    State s;
    s.run("Set(F, 'type', 'bare')");
    CHECK(s.run("return Get(F, '', 'type', '1')") == "bare");
    s.run("Set(F, '*type*', 'any')");
    CHECK(s.run("return Get(F, 'shift-', 'type', '1')") == "any");
    s.run("Set(F, 'shift-type*', 'button')");
    CHECK(s.run("return Get(F, 'shift-', 'type', '1')") == "button");
    s.run("Set(F, '*type1', 'modifier')");
    CHECK(s.run("return Get(F, 'Shift-', 'Type', '1')") == "modifier");
    s.run("Set(F, 'shift-type1', 'both')");
    CHECK(s.run("return Get(F, 'shift-', 'type', '1')") == "both");
    CHECK(s.run("return Get(F, nil, 'type', nil)") == "bare");
    // Two arguments are a name and something else.
    CHECK(s.run("return Get(F, 'type', 'x')") == "bare");
}

TEST_CASE("an attribute is secure whoever sets it, and nobody's as it is pushed", "[lua]") {
    State s;
    const int addon = taint::intern("SomeAddon");
    lua_getglobal(s.L, "F");
    {
        taint::Scope insecure(addon);
        lua_pushstring(s.L, "v");
        lua_settaintof(s.L, -1, addon);
        storeFrameAttribute(s.L, 1, "Name");
    }
    CHECK(lua_gettop(s.L) == 1);
    {
        taint::Scope secure(0);
        REQUIRE(pushFrameAttribute(s.L, 1, "name"));
        CHECK(lua_gettaintof(s.L, -1) == 0);
        CHECK(taint::current() == 0);
        lua_pop(s.L, 1);
    }
    {
        taint::Scope insecure(addon);
        REQUIRE(pushFrameAttribute(s.L, 1, "name"));
        CHECK(lua_gettaintof(s.L, -1) == addon);
        lua_pop(s.L, 1);
        // And what a handler is called with has nothing of the caller's.
        lua_pushstring(s.L, "NAME");
        lua_pushnumber(s.L, 5);
        lua_settaintof(s.L, 1, addon);
        pushAttributeChange(s.L, 1, lua_tostring(s.L, 2), 3);
        REQUIRE(lua_gettop(s.L) == 6);
        CHECK(lua_gettaintof(s.L, 4) == 0);
        CHECK(lua_gettaintof(s.L, 5) == 0);
        CHECK(lua_gettaintof(s.L, 6) == 0);
        CHECK(std::string(lua_tostring(s.L, 5)) == "name");
        CHECK(lua_tonumber(s.L, 6) == 5);
    }
    lua_settop(s.L, 0);
}
