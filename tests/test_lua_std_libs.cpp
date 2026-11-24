// The Lua libraries the interface has, against what Wow.exe 12340 registers:
// the base library's table 0x00a47c80, string 0x00a478e0, table 0x00a47798,
// math 0x00a47600, coroutine 0x00a47d28 and bit 0x00a475a0.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "addons/lua_arg_guard.hpp"
#include "addons/lua_std_libs.hpp"
#include "addons/lua_time.hpp"
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

TEST_CASE("the functions of other clients are taken out", "[lua]") {
    State s;
    s.run("for _, n in ipairs({'GetPlayerBuff', 'CancelPlayerBuff', 'GetSpellBookItemInfo', 'SaveMacros', 'GetNumLaguages', "
          "'AddSendMailCOD', 'IsSpellPassive', 'GetSpellInfo', 'IsPassiveSpell'}) do _G[n] = function() end end");
    wowee::addons::dropForeignGlobals(s.L);
    for (const char* gone : {"GetPlayerBuff", "CancelPlayerBuff", "GetSpellBookItemInfo", "SaveMacros", "GetNumLaguages",
                             "AddSendMailCOD", "IsSpellPassive"})
        CHECK(s.run(std::string("return type(") + gone + ")") == "nil");
    // The client's own stay.
    for (const char* kept : {"GetSpellInfo", "IsPassiveSpell"})
        CHECK(s.run(std::string("return type(") + kept + ")") == "function");
}

TEST_CASE("the getters raise their usage lines", "[lua]") {
    State s;
    s.run("UnitName = function(...) return 'called', ... end "
          "GetContainerItemInfo = function(...) return 'called', ... end "
          "GetLFDChoiceOrder = function(...) return 'called', select('#', ...) end "
          "GetItemInfo = function(...) return 'called', ... end");
    wowee::addons::guardArguments(s.L, wowee::addons::argGuards());
    const auto raises = [&s](const std::string& call, const std::string& line) {
        const std::string got = s.run("return " + call);
        return got.find(line) != std::string::npos && got.rfind("error: ", 0) == 0;
    };
    CHECK(raises("UnitName()", "Usage: UnitName(\"unit\")"));
    CHECK(raises("UnitName({})", "Usage: UnitName(\"unit\")"));
    CHECK(s.run("return UnitName('player')") == "called player");
    // Every argument asked for, and what goes past them handed on.
    CHECK(raises("GetContainerItemInfo(0)", "Usage: GetContainerItemInfo(index, slot)"));
    CHECK(raises("GetContainerItemInfo('bag', 1)", "Usage: GetContainerItemInfo(index, slot)"));
    CHECK(s.run("return GetContainerItemInfo(0, '3', nil, 4)") == "called 0 3 nil 4");
    // A table or nothing.
    CHECK(raises("GetLFDChoiceOrder(1)", "Usage: GetLFDChoiceOrder([table])"));
    CHECK(s.run("return GetLFDChoiceOrder()") == "called 0");
    CHECK(s.run("return GetLFDChoiceOrder({})") == "called 1");
    CHECK(raises("GetItemInfo()", "Usage: GetItemInfo(itemID|\"name\"|\"itemlink\")"));
    CHECK(s.run("return GetItemInfo(25)") == "called 25");
    // An error names the line that called.
    CHECK(s.run("return UnitName()").find(":1:") != std::string::npos);
    CHECK(s.run("return type(UnitHealth)") == "nil");   // not there, not made
}

TEST_CASE("the usage lines are whole", "[lua]") {
    std::set<std::string> names;
    for (const auto& guard : wowee::addons::argGuards()) {
        CHECK(names.insert(guard.name).second);
        const std::string arguments = guard.arguments;
        CHECK_FALSE(arguments.empty());
        CHECK(arguments.find_first_not_of("nsto") == std::string::npos);
        CHECK(std::string(guard.usage).rfind("Usage: ", 0) == 0);
    }
    CHECK(names.size() > 100);
}

TEST_CASE("time() takes a date table", "[lua]") {
    State s;
    lua_pushcfunction(s.L, wowee::addons::luaTimeOf);
    lua_setglobal(s.L, "time");
    const auto at = [](int year, int month, int day, int hour, int min, int sec) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        tm.tm_isdst = -1;
        return std::to_string(static_cast<long long>(std::mktime(&tm)));
    };
    CHECK(s.run("return time({year = 2010, month = 3, day = 14})") == at(2010, 3, 14, 12, 0, 0));
    CHECK(s.run("return time({year = 2010, month = 3, day = '14', hour = 1, min = 2, sec = 3})") == at(2010, 3, 14, 1, 2, 3));
    CHECK(s.run("return time({year = 2010, month = 3})").find("field 'day' missing in date table") != std::string::npos);
    CHECK(s.run("return time({month = 3, day = 4})").find("field 'year' missing in date table") != std::string::npos);
    CHECK(s.run("return time(5)").rfind("error: ", 0) == 0);
    CHECK(s.run("return time() > 1200000000, time(nil) > 1200000000") == "true true");
    CHECK(s.run("return time({year = 1800, month = 1, day = 1})") == "nil");
}

TEST_CASE("the getters answer for what is not there as the client's", "[lua]") {
    State s;
    s.run("UNKNOWN = 'Unbekannt' "
          "GetItemInfo = function(x) if x == 25 then return 'Schwert', 1 end return nil end "
          "GetFriendInfo = function() return nil end "
          "GetPetitionInfo = function() end "
          "GetLFGQueueStats = function() return false end "
          "GetSpellLink = function() return nil end "
          "GetMerchantItemInfo = function(i) if i == 2 then return nil, 'x' end return nil end "
          "GetCurrencyListInfo = function() return 'x' end "
          "GetSavedInstanceInfo = function() return nil end");
    wowee::addons::answerNothing(s.L, wowee::addons::nothingAnswers());
    CHECK(s.run("return select('#', GetItemInfo(1))") == "0");
    CHECK(s.run("return GetItemInfo(25)") == "Schwert 1");
    CHECK(s.run("return GetFriendInfo(1)") == "nil 0 Unbekannt Unbekannt nil  nil nil");
    CHECK(s.run("return select('#', GetFriendInfo(1))") == "8");
    CHECK(s.run("return GetPetitionInfo()") == "nil nil nil 0 nil nil 0");
    CHECK(s.run("return select('#', GetLFGQueueStats())") == "0");
    CHECK(s.run("return select('#', GetSpellLink(1))") == "2");
    CHECK(s.run("return select('#', GetSpellLink(1, 'spell'))") == "0");
    CHECK(s.run("return select('#', GetSpellLink('x'))") == "0");
    // A nil among others is an answer.
    CHECK(s.run("return GetMerchantItemInfo(2)") == "nil x");
    CHECK(s.run("return GetMerchantItemInfo(1)") == "nil nil 0 1 0 1 nil");
    CHECK(s.run("return GetSavedInstanceInfo(1)") == "nil 0 nil 0 false false 0 false 0 ");
    CHECK(s.run("return GetCurrencyListInfo(1)") == "x");
    CHECK(s.run("return type(GetRaidRosterInfo)") == "nil");
}
