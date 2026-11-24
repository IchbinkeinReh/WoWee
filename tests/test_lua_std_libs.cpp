// The Lua libraries the interface has, against what Wow.exe 12340 registers:
// the base library's table 0x00a47c80, string 0x00a478e0, table 0x00a47798,
// math 0x00a47600, coroutine 0x00a47d28 and bit 0x00a475a0.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "addons/lua_std_libs.hpp"
#include "addons/missing_api_rule.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>

namespace {

struct State {
    lua_State* L = luaL_newstate();
    State() { wowee::addons::openInterfaceLibs(L); }
    ~State() { lua_close(L); }
    std::string run(const std::string& chunk) {
        if (luaL_dostring(L, chunk.c_str()) != 0) {
            const std::string err = lua_tostring(L, -1);
            lua_settop(L, 0);
            return "error: " + err;
        }
        std::string out;
        for (int i = 1; i <= lua_gettop(L); ++i) {
            lua_getglobal(L, "tostring");
            lua_pushvalue(L, i);
            lua_call(L, 1, 1);
            out += (i > 1 ? " " : "") + std::string(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_settop(L, 0);
        return out;
    }
};

std::set<std::string> words(const std::string& text) {
    std::istringstream in(text);
    std::set<std::string> out;
    for (std::string w; in >> w;) out.insert(w);
    return out;
}

// The fields of a library table, or of _G.
std::set<std::string> fields(State& s, const std::string& table) {
    return words(s.run("local out = '' for k in pairs(" + table + ") do out = out .. ' ' .. tostring(k) end return out"));
}

}  // namespace

TEST_CASE("the interface's libraries are the client's", "[lua]") {
    State s;
    // What the table is made of there, and here as well as of what Lua itself
    // puts there (pi, huge and mod in math, gfind's and setn's old names).
    const std::set<std::string> string = words("byte char dump find format gfind gmatch gsub len lower match rep reverse sub upper");
    CHECK(fields(s, "string") == string);
    CHECK(fields(s, "table") == words("concat foreach foreachi getn maxn insert remove removemulti setn sort"));
    CHECK(fields(s, "coroutine") == words("create resume running status wrap yield"));
    CHECK(fields(s, "bit") == words("bnot band bor bxor lshift rshift arshift mod"));
    const std::set<std::string> math = fields(s, "math");
    for (const auto& name : words("abs acos asin atan2 atan ceil cosh cos deg exp floor fmod frexp ldexp log10 log max min modf "
                                  "pow rad random sinh sin sqrt tanh tan pi huge"))
        CHECK(math.count(name) == 1);
    CHECK(math.count("randomseed") == 0);
    for (const auto& name : words("assert collectgarbage error gcinfo getfenv getmetatable loadstring next pcall rawequal rawget "
                                  "rawset select setfenv setmetatable tonumber tostring type unpack xpcall ipairs pairs newproxy"))
        CHECK(s.run("return type(" + name + ")") == "function");
    for (const auto& name : words("dofile loadfile load io os debug package require module"))
        CHECK(s.run("return type(" + name + ")") == "nil");
}

TEST_CASE("collectgarbage answers as the client's does", "[lua]") {
    State s;
    // 0x008545c0: kilobytes with their rest for "count", a number else, a
    // yes or no for "step".
    CHECK(s.run("return type(collectgarbage('count')), collectgarbage('count') > 0") == "number true");
    CHECK(s.run("return type(collectgarbage()), type(collectgarbage('step'))") == "number boolean");
    CHECK(s.run("collectgarbage('stop') collectgarbage('restart') return 'ok'") == "ok");
    CHECK(s.run("return collectgarbage('rubbish')").find("invalid option 'rubbish'") != std::string::npos);
    CHECK(s.run("local before = collectgarbage('count') local t = {} for i = 1, 20000 do t[i] = {} end "
                "local with = collectgarbage('count') t = nil collectgarbage('collect') "
                "return with > before, collectgarbage('count') < with") == "true true");
}

TEST_CASE("bit works on the low 32 bits, and shifts count modulo 32", "[lua]") {
    State s;
    CHECK(s.run("return bit.bor(1, 16, 256, 1024, 16384)") == "17681");
    CHECK(s.run("return bit.bor(5), bit.band(255, 15, 3), bit.bxor(1, 3, 4)") == "5 3 6");
    CHECK(s.run("return bit.bor(0x80000000, 1), bit.band(-1, 255), bit.bnot(0)") == "2147483649 255 4294967295");
    CHECK(s.run("return bit.lshift(1, 4), bit.rshift(0x4511, 8), bit.lshift(1, 31)") == "16 69 2147483648");
    CHECK(s.run("return bit.rshift(-1, 1), bit.arshift(-8, 1), bit.arshift(8, 1)") == "2147483647 -4 4");
    // 32 is none, and 33 one.
    CHECK(s.run("return bit.lshift(1, 32), bit.rshift(4, 33), bit.arshift(4, 33)") == "1 2 2");
    CHECK(s.run("return bit.mod(7, 3), bit.mod(-7, 3), bit.mod(7, -3)") == "1 -1 1");
    CHECK(s.run("return bit.mod(5, 0.5)") == "2");
    CHECK(s.run("return bit.mod(-2147483648, -1)") == "0");
    CHECK(s.run("return bit.band('12', 8)") == "8");
    CHECK(s.run("return bit.band({}, 1)").find("number expected") != std::string::npos);
    // Past what a double's whole number holds there are no bits.
    CHECK(s.run("return bit.band(1e300, 255), bit.bor(0/0, 4)") == "0 4");
}

TEST_CASE("removemulti takes a run out of a list", "[lua]") {
    State s;
    // The values it takes out, and what is left.
    CHECK(s.run("local t = {1, 2, 3, 4, 5} local a, b, c = table.removemulti(t, 2, 2) "
                "return a, b, c, table.concat(t, ',')") == "2 3 nil 1,4,5");
    CHECK(s.run("local t = {1, 2, 3} local a, b = table.removemulti(t) return a, b, table.concat(t, ',')") == "3 nil 1,2");
    CHECK(s.run("local t = {1, 2, 3} local a, b, c = table.removemulti(t, 1, 3) return a, b, c, #t") == "1 2 3 0");
    CHECK(s.run("local t = {1, 2} return select('#', table.removemulti(t, 1, 0)), table.concat(t, ',')") == "0 1,2");
    CHECK(s.run("return table.removemulti({})").empty());
    for (const char* bad : {"0, 1", "4, 1", "3, 2", "1, -1", "2, 2147483647"})
        CHECK(s.run(std::string("return table.removemulti({1, 2, 3}, ") + bad + ")").find("parameters out of bounds") != std::string::npos);
}

TEST_CASE("the string methods and the old loops are set from the libraries", "[lua]") {
    State s;
    s.run("strtrim = function(t) return (t:gsub('^%s+', ''):gsub('%s+$', '')) end "
          "strsplit = function(d, t) return t:match('^(.-)' .. d .. '(.*)$') end "
          "strjoin = function(d, a, b) return a .. d .. b end "
          "strreplace = function(t, a, b) return (t:gsub(a, b)) end");
    wowee::addons::setInterfaceLibAliases(s.L);
    CHECK(s.run("return ('  a '):trim(), string.split(',', 'a,b')") == "a a b");
    CHECK(s.run("return string.join('-', 'a', 'b'), ('aXb'):replace('X', 'y')") == "a-b ayb");
    CHECK(s.run("return foreach == table.foreach, foreachi == table.foreachi") == "true true");
}

TEST_CASE("a global nobody set is nil in the 3.3.5 interface", "[lua]") {
    using wowee::addons::missingApiStandsIn;
    CHECK_FALSE(missingApiStandsIn("", "", false));
    CHECK_FALSE(missingApiStandsIn("", "1", false));
    // Asked for, it is there; and an older expansion's interface has it.
    CHECK(missingApiStandsIn("1", "", false));
    CHECK(missingApiStandsIn("", "", true));
    CHECK_FALSE(missingApiStandsIn("", "0", true));
    CHECK_FALSE(missingApiStandsIn("0", "", true));
}
