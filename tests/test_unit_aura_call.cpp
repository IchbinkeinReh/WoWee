// The UnitAura call GameTooltip:SetUnitAura makes (addons/unit_aura_call.hpp).
//
// SetUnitAura("player", 1) - no filter - built nothing while SetUnitBuff, which
// always passes one, worked: the arguments were copied from fixed slots after
// UnitAura had been pushed, so the missing filter slot held UnitAura itself and
// UnitAura raised on a function where its filter goes. These run the call
// against a stand-in UnitAura that records what it was handed.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <string>
#include <vector>

#include "addons/unit_aura_call.hpp"

namespace {

std::vector<std::string> seen;

/// UnitAura as the client's raises it on a filter that is not text, and
/// otherwise answers eleven values, the first naming the filter it read.
int fakeUnitAura(lua_State* L) {
    seen.clear();
    for (int i = 1; i <= 4; ++i) seen.emplace_back(luaL_typename(L, i));
    const char* filter = luaL_optstring(L, 3, "HELPFUL");
    lua_pushstring(L, filter);
    for (int i = 1; i < 11; ++i) lua_pushnumber(L, i);
    return 11;
}

struct State {
    lua_State* L = luaL_newstate();
    State() {
        lua_pushcfunction(L, fakeUnitAura);
        lua_setglobal(L, "UnitAura");
    }
    ~State() { lua_close(L); }
};

}  // namespace

TEST_CASE("SetUnitAura with no filter hands UnitAura nil for it", "[tooltip][aura]") {
    State s;
    lua_newtable(s.L);               // self
    lua_pushstring(s.L, "player");
    lua_pushnumber(s.L, 1);
    REQUIRE(wowee::addons::callUnitAuraForSetter(s.L));
    REQUIRE(seen.size() == 4);
    CHECK(seen[0] == "string");
    CHECK(seen[1] == "number");
    CHECK(seen[2] == "nil");
    CHECK(seen[3] == "nil");
    // UnitAura's default, HELPFUL, is what it read.
    CHECK(std::string(lua_tostring(s.L, -11)) == "HELPFUL");
    CHECK(lua_gettop(s.L) == 5 + 11);
}

TEST_CASE("SetUnitAura passes a filter it was given", "[tooltip][aura]") {
    State s;
    lua_newtable(s.L);
    lua_pushstring(s.L, "target");
    lua_pushnumber(s.L, 2);
    lua_pushstring(s.L, "HARMFUL|PLAYER");
    REQUIRE(wowee::addons::callUnitAuraForSetter(s.L));
    CHECK(seen[2] == "string");
    CHECK(std::string(lua_tostring(s.L, -11)) == "HARMFUL|PLAYER");
}

TEST_CASE("SetUnitAura by name passes the rank and the filter after it", "[tooltip][aura]") {
    State s;
    lua_newtable(s.L);
    lua_pushstring(s.L, "player");
    lua_pushstring(s.L, "Power Word: Fortitude");
    lua_pushstring(s.L, "Rank 8");
    lua_pushstring(s.L, "HELPFUL");
    REQUIRE(wowee::addons::callUnitAuraForSetter(s.L));
    CHECK(seen[1] == "string");
    CHECK(seen[2] == "string");
    CHECK(seen[3] == "string");
}

TEST_CASE("a UnitAura that raises leaves nothing behind", "[tooltip][aura]") {
    State s;
    lua_newtable(s.L);
    lua_pushstring(s.L, "player");
    lua_pushnumber(s.L, 1);
    lua_newtable(s.L);               // a table where the filter goes
    CHECK_FALSE(wowee::addons::callUnitAuraForSetter(s.L));
    CHECK(lua_gettop(s.L) == 5);
}

TEST_CASE("no UnitAura at all is no aura", "[tooltip][aura]") {
    lua_State* L = luaL_newstate();
    lua_newtable(L);
    lua_pushstring(L, "player");
    lua_pushnumber(L, 1);
    CHECK_FALSE(wowee::addons::callUnitAuraForSetter(L));
    CHECK(lua_gettop(L) == 5);
    lua_close(L);
}
