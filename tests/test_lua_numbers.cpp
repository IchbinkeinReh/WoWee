// Numbers and texts of the language itself: extern/lua-5.1.5 (lobject.c,
// lstrlib.c) and addons/lua_text_functions.hpp.
#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
extern int luaStr_decimalcomma;
}

#include "addons/lua_text_functions.hpp"
#include "core/crt_round.hpp"

namespace {
struct Lua {
    lua_State* L = luaL_newstate();
    Lua() {
        for (lua_CFunction open : {luaopen_base, luaopen_string, luaopen_table}) {
            lua_pushcfunction(L, open);
            lua_call(L, 0, 0);
        }
        lua_register(L, "strjoin", wowee::addons::luaStrJoin);
        lua_register(L, "date", wowee::addons::luaDate);
        lua_getglobal(L, "string");
        lua_pushcfunction(L, wowee::addons::luaStrJoin);
        lua_setfield(L, -2, "join");
        lua_pop(L, 1);
    }
    ~Lua() { lua_close(L); }
    /// What an expression is, as text; its error with a "!" before it.
    std::string operator()(const std::string& expression) {
        lua_settop(L, 0);
        const std::string code = "return tostring(" + expression + ")";
        if (luaL_loadstring(L, code.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0)
            return std::string("!") + lua_tostring(L, -1);
        return lua_tostring(L, -1);
    }
};
}  // namespace

TEST_CASE("a number is written as the client's runtime does", "[lua][number]") {
    Lua lua;
    CHECK(lua("0/0") == "-1.#IND");
    CHECK(lua("-(0/0)") == "1.#QNAN");
    CHECK(lua("1/0") == "1.#INF");
    CHECK(lua("-1/0") == "-1.#INF");
    CHECK(lua("1e308*10") == "1.#INF");
    CHECK(lua("1e15") == "1e+015");
    CHECK(lua("1e100") == "1e+100");
    CHECK(lua("0.00001") == "1e-005");
    CHECK(lua("-2.5e-7") == "-2.5e-007");
    CHECK(lua("1.5") == "1.5");
    CHECK(lua("100") == "100");
    CHECK(lua("12345678901234") == "12345678901234");
    CHECK(lua("123456789012345") == "1.2345678901235e+014");
    CHECK(lua("'a'..(1/0)") == "a1.#INF");
}

TEST_CASE("a text that is no number to the client", "[lua][number]") {
    Lua lua;
    CHECK(lua("tonumber('inf')") == "nil");
    CHECK(lua("tonumber('nan')") == "nil");
    CHECK(lua("tonumber('-infinity')") == "nil");
    CHECK(lua("tonumber('0x10')") == "16");
    CHECK(lua("tonumber(' 0xFf ')") == "255");
    CHECK(lua("tonumber('-0x10')") == "4294967280");
    CHECK(lua("tonumber('0x1ffffffff')") == "4294967295");
    CHECK(lua("tonumber('0x1p4')") == "nil");
    CHECK(lua("tonumber('0x')") == "nil");
    CHECK(lua("tonumber('1.#INF')") == "nil");
    CHECK(lua("tonumber(' -1.5e2 ')") == "-150");
    CHECK(lua("tonumber('.5')") == "0.5");
    CHECK(lua("tonumber('1e')") == "nil");
    CHECK(lua("0x20 + 1") == "33");
}

TEST_CASE("format writes numbers as the client's", "[lua][format]") {
    Lua lua;
    const auto f = [&](const std::string& args) { return lua("string.format(" + args + ")"); };
    CHECK(f("'%f', 1/0") == "1.#INF00");
    CHECK(f("'%.2f', 1/0") == "1.#J");
    CHECK(f("'%.1f', 1/0") == "1.$");
    CHECK(f("'%.0f', 1/0") == "1");
    CHECK(f("'%.2f', 0/0") == "-1.#J");
    CHECK(f("'%e', 1/0") == "1.#INF00e+000");
    CHECK(f("'%g', -1/0") == "-1.#INF");
    CHECK(f("'%8.3f|', 1/0") == "   1.#IO|");
    CHECK(f("'%g', 1e6") == "1e+006");
    CHECK(f("'%e', 12345.678") == "1.234568e+004");
    CHECK(f("'%12.2E|', 12345.678") == "   1.23E+004|");
    CHECK(f("'%-12.2e|', -12345.678") == "-1.23e+004  |");
    CHECK(f("'%012.2e|', -12345.678") == "-001.23e+004|");
    CHECK(f("'%5.1f|%-6.2f|%06.1f|%+d|%+.1f', 1.25, 2.5, -3.5, 7, 0.5") == "  1.3|2.50  |-003.5|+7|+0.5");
    CHECK(f("'%#.0f', 3") == "3.");
    CHECK(f("'%d', 2^31") == "-2147483648");
    CHECK(f("'%d', 2^32 + 5") == "5");
    CHECK(f("'%d', -3.9") == "-3");
    CHECK(f("'%x', 2^32 - 1") == "ffffffff");
    CHECK(f("'%u', -1") == "4294967295");
    luaStr_decimalcomma = 1;
    CHECK(f("'%.2F|%.2f', 1.5, 1.5") == "1,50|1.50");
    luaStr_decimalcomma = 0;
    CHECK(f("'%.2F', 1.5") == "1.50");
}

// 0x00853c50
TEST_CASE("format takes its arguments by place", "[lua][format]") {
    Lua lua;
    const auto f = [&](const std::string& args) { return lua("string.format(" + args + ")"); };
    CHECK(f("'%2$s %1$s', 'a', 'b'") == "b a");
    CHECK(f("'%2$s %s', 'a', 'b', 'c'") == "b c");
    CHECK(f("'%1$s %1$s %s', 'a', 'b'") == "a a b");
    CHECK(f("'%12$s', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 'x'") == "x");
    CHECK(f("'%1$.1f%% %2$5d|', 2.55, 7") == "2.5%     7|");
    CHECK(f("'100%%2$s'") == "100%2$s");
    CHECK(f("'%5s|', 'a'") == "    a|");
    CHECK(f("'%s', nil").find("bad argument #2 to 'format'") != std::string::npos);
    CHECK(f("'%2$s', 'a'").find("bad argument #3 to 'format'") != std::string::npos);
    CHECK(f("'%y', 1")[0] == '!');
}

// 0x00816b60
TEST_CASE("strjoin puts its separator between the pieces", "[lua][strjoin]") {
    Lua lua;
    CHECK(lua("strjoin('-', 'a', 'b', 'c')") == "a-b-c");
    CHECK(lua("strjoin(', ', 'x', 'y')") == "x, y");
    CHECK(lua("strjoin(', ', 'x')") == "x");
    CHECK(lua("strjoin(', ')") == "");
    CHECK(lua("strjoin('', 'x', 1, 'y')") == "x1y");
    CHECK(lua("('-'):join('a', 'b', 3, 'd')") == "a-b-3-d");
    CHECK(lua("strjoin('-', 'a', nil)")[0] == '!');
}

// 0x00817450
TEST_CASE("date with a ! is Greenwich's", "[lua][date]") {
    Lua lua;
    CHECK(lua("date('!%H:%M', 3600)") == "01:00");
    CHECK(lua("date('!%d.%m.%Y %H:%M:%S', 1234567890)") == "13.02.2009 23:31:30");
    CHECK(lua("date('!*t', 3600).hour") == "1");
    CHECK(lua("date('*t', 1234567890).year") == "2009");
    CHECK(lua("type(date('*t').isdst)") == "boolean");
    CHECK(lua("date('')").find("'date' format too long") != std::string::npos);
    CHECK(lua("date('%Y', 'x')")[0] == '!');
}

// 0x00414ac5: the digits are rounded, so an exact half goes up; the system's goes to the even.
TEST_CASE("what is half way goes up", "[lua][number]") {
    Lua lua;
    CHECK(lua("string.format('%.0f %.0f %.0f %.0f', .5, 1.5, 2.5, -.5)") == "1 2 3 -1");
    CHECK(lua("string.format('%.1f %.1f %.1f', 2.25, 12.25, .75)") == "2.3 12.3 0.8");
    CHECK(lua("string.format('%.2f %.2f %.2f', .125, 1.005, 2.675)") == "0.13 1.00 2.67");
    CHECK(lua("string.format('%.1f %.1f %.1f', .05, .35, 2.5)") == "0.1 0.3 2.5");
    CHECK(lua("string.format('%6.1f|%-6.1f|%+.1f', 2.25, 2.25, 2.25)") == "   2.3|2.3   |+2.3");
    CHECK(lua("string.format('%.1e %.2g %.2g %.3g', 2.25, 125, 135, 1.125)") == "2.3e+000 1.3e+002 1.4e+002 1.13");
    CHECK(lua("string.format('%.0f %.1f', 1e15 + .5, 0)") == "1000000000000001 0.0");
    CHECK(lua("0.5") == "0.5");
    CHECK(lua("1/8192") == "0.0001220703125");
}

TEST_CASE("the client's rounding for what is printed besides", "[number]") {
    using wowee::core::halfUp;
    const auto text = [](double v, int decimals) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, halfUp(v, decimals));
        return std::string(buf);
    };
    CHECK(text(12.25, 1) == "12.3");
    CHECK(text(-12.25, 1) == "-12.3");
    CHECK(text(12.35, 1) == "12.3");
    CHECK(text(12.75, 1) == "12.8");
    CHECK(text(0.5, 0) == "1");
    CHECK(text(1.125, 2) == "1.13");
    CHECK(text(3.0, 1) == "3.0");
    CHECK(halfUp(1.0 / 3.0, 1) == 1.0 / 3.0);
}
