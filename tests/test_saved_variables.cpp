// What a SavedVariables file holds (addons/saved_variables.hpp; 0x008183a0).
#include <catch_amalgamated.hpp>

#include "addons/saved_variables.hpp"

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <string>

using namespace wowee::addons;

namespace {
struct Lua {
    lua_State* L = luaL_newstate();
    Lua() {
        lua_pushcfunction(L, luaopen_base);
        lua_call(L, 0, 0);
    }
    ~Lua() { lua_close(L); }
    void run(const std::string& code) {
        INFO(code);
        if (luaL_loadbuffer(L, code.data(), code.size(), "=test") != 0 || lua_pcall(L, 0, 0, 0) != 0)
            FAIL(lua_tostring(L, -1));
    }
    std::string value(const std::string& expression) {
        run("V = " + expression);
        lua_getglobal(L, "V");
        std::string out;
        saved_variables::appendValue(L, -1, out);
        lua_pop(L, 1);
        CHECK(lua_gettop(L) == 0);
        return out;
    }
    bool truth(const std::string& expression) {
        run("T = " + expression);
        lua_getglobal(L, "T");
        const bool out = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return out;
    }
};
}  // namespace

TEST_CASE("what is saved of a value", "[saved_variables]") {
    Lua l;
    CHECK(l.value("nil") == "nil");
    CHECK(l.value("true") == "true");
    CHECK(l.value("false") == "false");
    CHECK(l.value("1.5") == "1.5");
    CHECK(l.value("1e20") == "1e+20");
    CHECK(l.value("0.1") == "0.1");
    CHECK(l.value("1/0") == "nil --[[ inf ]]");
    CHECK(l.value("-1/0") == "nil --[[ inf ]]");
    CHECK(l.value("0/0") == "nil --[[ nan ]]");
    CHECK(l.value("print") == "nil --[[ skipped inline function ]]");
    CHECK(l.value("newproxy()") == "nil --[[ skipped userdata ]]");
    CHECK(l.value("'a\\000b\\r\\nc\\\\d\"e'") == "\"a\\000b\\r\\nc\\\\d\\\"e\"");
    CHECK(l.value("{}") == "{\r\n}");
}

TEST_CASE("what is saved of a table", "[saved_variables]") {
    Lua l;
    CHECK(l.value("{ 'a', 2, { 'b' } }") ==
          "{\r\n\t\"a\", -- [1]\r\n\t2, -- [2]\r\n\t{\r\n\t\t\"b\", -- [1]\r\n\t}, -- [3]\r\n}");
    // The numbers past the list, and those between.
    CHECK(l.value("{ 'a', 'b', [1.5] = 'half' }") == "{\r\n\t\"a\", -- [1]\r\n\t\"b\", -- [2]\r\n\t[1.5] = \"half\",\r\n}");
    CHECK(l.value("{ 'a', [-3] = 'x' }") == "{\r\n\t\"a\", -- [1]\r\n\t[-3] = \"x\",\r\n}");
    CHECK(l.value("{ [true] = 1 }") == "{\r\n\t[true] = 1,\r\n}");
    CHECK(l.value("{ ['k\\n'] = { x = 1/0 } }") ==
          "{\r\n\t[\"k\\n\"] = {\r\n\t\t[\"x\"] = nil --[[ inf ]],\r\n\t},\r\n}");
    CHECK(l.value("{ [1/0] = 1 }") == "{\r\n\t--[[ skipped entry with inf key ]]\r\n}");
    CHECK(l.value("{ [print] = 1 }") == "{\r\n\t--[[ skipped entry with function key ]]\r\n}");
}

TEST_CASE("a table within itself, and one that is there twice", "[saved_variables]") {
    Lua l;
    l.run("A = {} A.self = A");
    CHECK(l.value("A") == "{\r\n\t[\"self\"] = nil --[[ skipped recursive table ]],\r\n}");
    l.run("B = { x = 1 } C = { B, B }");
    CHECK(l.value("C") == "{\r\n\t{\r\n\t\t[\"x\"] = 1,\r\n\t}, -- [1]\r\n\t{\r\n\t\t[\"x\"] = 1,\r\n\t}, -- [2]\r\n}");
    // Deeper than Lua reads: cut, and read back as far as it goes.
    l.run("D = {} local t = D for i = 1, 20000 do t.t = {} t = t.t end");
    l.run(saved_variables::fileText(l.L, {"D"}));
    l.run("depth = 0 while D do D = D.t depth = depth + 1 end");
    CHECK(l.truth("depth == 150"));
}

TEST_CASE("a file is read back as it was", "[saved_variables]") {
    Lua l;
    l.run("DB = { 'a', [1.5] = 'half', [7] = 'seven', [true] = 'yes', [false] = 'no', s = 'x\\000y\\r\\n\\'\"\\\\',"
          " n = 0.1, big = 1e300, sub = { 1, nil, 3, deep = { {} } } } DB.me = DB NUM = 5 GONE = nil");
    const std::string text = saved_variables::fileText(l.L, {"DB", "NUM", "GONE", "print()", ""});
    CHECK(text.rfind("\r\nDB = {\r\n", 0) == 0);
    CHECK(text.find("\r\nNUM = 5\r\nGONE = nil\r\n") != std::string::npos);
    CHECK(text.find("print") == std::string::npos);
    CHECK(text.substr(text.size() - 2) == "\r\n");
    l.run("OLD = DB OLD.me = nil DB, NUM, GONE = nil, nil, 4");
    l.run(text);
    l.run("function same(a, b) if type(a) ~= 'table' then return a == b end"
          " for k, v in pairs(a) do if not same(v, b[k]) then return false end end"
          " for k in pairs(b) do if a[k] == nil then return false end end return true end");
    CHECK(l.truth("same(OLD, DB) and NUM == 5 and GONE == nil"));
}

TEST_CASE("an object's inner fields are not written", "[saved_variables]") {
    Lua l;
    l.run("F = { [0] = newproxy(), __wid = 7, __scripts = {}, __own_thing = 1, mine = 2 }");
    const std::string text = l.value("F");
    CHECK(text.find("__wid") == std::string::npos);
    CHECK(text.find("__scripts") == std::string::npos);
    CHECK(text.find("[\"__own_thing\"] = 1") != std::string::npos);
    CHECK(text.find("[\"mine\"] = 2") != std::string::npos);
    CHECK(text.find("[0] = nil --[[ skipped userdata ]]") != std::string::npos);
    // Of no object they are an addon's own.
    CHECK(l.value("{ __scripts = 1 }") == "{\r\n\t[\"__scripts\"] = 1,\r\n}");
    CHECK(std::is_sorted(std::begin(inner_fields::kNames), std::end(inner_fields::kNames)));
}

TEST_CASE("which names are written", "[saved_variables]") {
    CHECK(saved_variables::nameWritten("My_DB2"));
    CHECK(saved_variables::nameWritten("B\xc3\xbcro"));
    CHECK_FALSE(saved_variables::nameWritten("a.b"));
    CHECK_FALSE(saved_variables::nameWritten("a b"));
    CHECK_FALSE(saved_variables::nameWritten(""));
}
