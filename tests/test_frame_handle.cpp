// frame[0] and the methods called by it (0x004a81b0), as the restricted frames
// call them.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "addons/frame_handle.hpp"
#include "addons/lua_std_libs.hpp"
#include "addons/lua_taint.hpp"

#include <string>

namespace {

int getName(lua_State* L) {
    lua_getfield(L, 1, "__name");
    return 1;
}

struct State {
    lua_State* L = luaL_newstate();
    State() {
        wowee::addons::openInterfaceLibs(L);
        lua_newtable(L);
        wowee::addons::pushMethodByHandle(L, getName);
        lua_setfield(L, -2, "GetName");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_setglobal(L, "MT");
        frame("F1", 5);
        frame("F2", 6);
    }
    ~State() { lua_close(L); }
    void frame(const char* name, uint32_t widget) {
        lua_newtable(L);
        lua_pushinteger(L, static_cast<lua_Integer>(widget));
        lua_setfield(L, -2, "__wid");
        wowee::addons::setFrameHandle(L, widget);
        lua_pushstring(L, name);
        lua_setfield(L, -2, "__name");
        lua_getglobal(L, "MT");
        lua_setmetatable(L, -2);
        lua_setglobal(L, name);
    }
    std::string run(const std::string& chunk) {
        if (luaL_dostring(L, chunk.c_str()) != 0) {
            const std::string err = lua_tostring(L, -1);
            lua_settop(L, 0);
            return "error: " + err;
        }
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        const std::string out = lua_tostring(L, -1);
        lua_settop(L, 0);
        return out;
    }
};

}  // namespace

TEST_CASE("a frame has its object at 0", "[lua]") {
    State s;
    CHECK(s.run("return type(F1[0])") == "userdata");
    CHECK(s.run("return F1[0] == F2[0]") == "false");
    CHECK(wowee::addons::widgetOfHandle(nullptr) == 0);
    int other = 0;
    CHECK(wowee::addons::widgetOfHandle(&other) == 0);
}

TEST_CASE("a method is called on whatever has the frame's handle", "[lua]") {
    State s;
    CHECK(s.run("return F1:GetName()") == "F1");
    // As restrictedframes.lua makes one.
    CHECK(s.run("local t = {[0] = F2[0], [1] = F2} setmetatable(t, getmetatable(F2)) return t:GetName()") == "F2");
    CHECK(s.run("return F1.GetName({[0] = F2[0]})") == "F2");
    // The frame's own table is itself, whatever is at 0 of it.
    CHECK(s.run("F1[0] = F2[0] return F1:GetName()") == "F1");
    // No handle, and one of nothing that is there.
    CHECK(s.run("return F1.GetName({__name = 'own'})") == "own");
    CHECK(s.run("return F1.GetName({[0] = 3, __name = 'own'})") == "own");
    wowee::addons::setFrameHandle((lua_newtable(s.L), s.L), 7);
    lua_rawgeti(s.L, -1, 0);
    lua_setglobal(s.L, "H7");
    lua_settop(s.L, 0);
    CHECK(s.run("return F1.GetName({[0] = H7, __name = 'own'})") == "nil");   // the table behind it has no name
}

TEST_CASE("a method written in Lua is called so too", "[lua]") {
    State s;
    s.run("MT.Own = 5 function MT:SetText(a, ...) self.text = a return self.__name, select('#', ...), ... end");
    lua_getglobal(s.L, "MT");
    wowee::addons::makeMethodsByHandle(s.L);
    CHECK(lua_gettop(s.L) == 1);
    lua_settop(s.L, 0);
    CHECK(s.run("return F1:SetText('x')") == "F1");
    CHECK(s.run("return F1.SetText({[0] = F2[0]}, 'y')") == "F2");
    CHECK(s.run("return F1.text .. F2.text") == "xy");
    CHECK(s.run("return select(2, F1:SetText('x', 1, nil, nil))") == "3");
    CHECK(s.run("return select('#', F1:SetText('x', 1, nil, nil))") == "5");
    // What is no method stays, and the methods in C as they are.
    CHECK(s.run("return MT.Own") == "5");
    CHECK(s.run("return F1.GetName({[0] = F2[0]})") == "F2");
    // One at a time, whichever it is written in.
    lua_getglobal(s.L, "MT");
    lua_pushcfunction(s.L, getName);
    wowee::addons::makeMethodByHandle(s.L);
    lua_setfield(s.L, -2, "Again");
    lua_settop(s.L, 0);
    CHECK(s.run("return F1.Again({[0] = F2[0]})") == "F2");
}

TEST_CASE("a handle taints nobody and is nobody's", "[lua]") {
    State s;
    namespace taint = wowee::addons::taint;
    const int addon = taint::intern("SomeAddon");
    {
        taint::Scope insecure(addon);
        s.frame("F3", 8);
    }
    lua_getglobal(s.L, "F3");
    lua_rawgeti(s.L, -1, 0);
    CHECK(lua_gettaintof(s.L, -1) == 0);
    lua_settop(s.L, 0);
    {
        taint::Scope secure(0);
        lua_newtable(s.L);
        lua_getglobal(s.L, "F1");
        lua_rawgeti(s.L, -1, 0);
        lua_rawseti(s.L, -3, 0);
        lua_settop(s.L, 1);
        wowee::addons::takeSelfByHandle(s.L);
        CHECK(taint::current() == 0);
        CHECK(lua_gettaintof(s.L, 1) == 0);
        lua_getfield(s.L, 1, "__name");
        CHECK(std::string(lua_tostring(s.L, -1)) == "F1");
        lua_settop(s.L, 0);
    }
}

TEST_CASE("the table behind a widget", "[lua]") {
    State s;
    wowee::addons::pushFrameByHandle(s.L, 6);
    lua_getfield(s.L, -1, "__name");
    CHECK(std::string(lua_tostring(s.L, -1)) == "F2");
    lua_settop(s.L, 0);
    wowee::addons::pushFrameByHandle(s.L, 99);
    CHECK(lua_gettop(s.L) == 1);
    CHECK(lua_isnil(s.L, 1));
    lua_settop(s.L, 0);
    const int addon = wowee::addons::taint::intern("SomeAddon");
    {
        wowee::addons::taint::Scope insecure(addon);
        wowee::addons::pushFrameByHandle(s.L, 6);
        CHECK(lua_gettaintof(s.L, -1) == addon);
    }
}

// 0x004a1f10, 0x004a2510: what GetRegions and GetChildren answer.
TEST_CASE("the objects of some widgets", "[lua]") {
    State s;
    lua_settop(s.L, 0);
    s.frame("F3", 9);
    REQUIRE(wowee::addons::pushObjects(s.L, {9, 7, 5}) == 2);
    REQUIRE(lua_gettop(s.L) == 2);
    const auto name = [&](int at) {
        lua_getfield(s.L, at, "__name");
        const std::string out = lua_tostring(s.L, -1) ? lua_tostring(s.L, -1) : "";
        lua_pop(s.L, 1);
        return out;
    };
    CHECK(name(1) == "F3");
    CHECK(name(2) == "F1");
    CHECK(wowee::addons::pushObjects(s.L, {}) == 0);
    CHECK(wowee::addons::pushObjects(s.L, std::vector<uint32_t>(2000000, 5)) == -1);
    CHECK(lua_gettop(s.L) == 2);
}
