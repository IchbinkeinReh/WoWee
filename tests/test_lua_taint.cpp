// The client's add-on protection: the taint each value carries, how running
// code picks it up, and what 0x005191c0 refuses because of it.
//
// None of this needs a window or a game. The Lua core carries the taint (see
// lua_TaintState in extern/lua-5.1.5/src/lua.h) and lua_taint.cpp holds the
// rest - the bracket the client puts around every call into Lua, the
// interface's hardware-event and lockdown flags, the check and its report -
// so what has to be exactly right can be asked of a bare Lua state.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <string>
#include <utility>
#include <vector>

#include "addons/lua_handler_globals.hpp"
#include "addons/lua_taint.hpp"

namespace taint = wowee::addons::taint;

namespace {

using Fired = std::vector<std::pair<std::string, std::vector<std::string>>>;

/// A Lua state with the taint vocabulary registered, a protected function of
/// each sort, and a record of what the check reported.
struct Fixture {
    lua_State* L = luaL_newstate();
    Fired fired;
    int addon = taint::intern("SomeAddon");

    Fixture() {
        taint::reset();
        lua_pushcfunction(L, luaopen_base);
        lua_pushstring(L, "");
        lua_call(L, 1, 0);
        taint::registerLuaApi(L);
        taint::setEventSink([this](const std::string& e, const std::vector<std::string>& a) {
            fired.emplace_back(e, a);
        });
        // A protected function of each rule, the way a binding asks.
        lua_register(L, "Forbidden", [](lua_State* S) -> int {
            lua_pushboolean(S, taint::allowed(S, 2));
            return 1;
        });
        lua_register(L, "NeedsClick", [](lua_State* S) -> int {
            lua_pushboolean(S, taint::allowed(S, 7));
            return 1;
        });
        lua_register(L, "NotInCombat", [](lua_State* S) -> int {
            lua_pushboolean(S, taint::allowed(S, 0xe));
            return 1;
        });
        lua_register(L, "Movement", [](lua_State* S) -> int {
            lua_pushboolean(S, taint::allowed(S, 0));
            return 1;
        });
        // What a handler's running taint is, by name.
        lua_register(L, "Taint", [](lua_State* S) -> int {
            const char* n = taint::name(taint::current());
            if (n) lua_pushstring(S, n);
            else lua_pushnil(S);
            return 1;
        });
    }
    ~Fixture() {
        taint::setEventSink(nullptr);
        lua_close(L);
        taint::reset();
    }

    /// Run a chunk as the client runs one: secure (Blizzard's) for 0, or as
    /// the named addon's file.
    void run(const char* code, int as) {
        taint::Scope scope(as, as);
        INFO(code);
        REQUIRE(luaL_dostring(L, code) == 0);
    }

    bool globalBool(const char* name) {
        lua_getglobal(L, name);
        const bool b = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return b;
    }
    std::string globalString(const char* name) {
        lua_getglobal(L, name);
        const char* s = lua_tostring(L, -1);
        std::string out = s ? s : "<nil>";
        lua_pop(L, 1);
        return out;
    }
    int globalTaint(const char* name) {
        taint::Quiet quiet;
        lua_getglobal(L, name);
        const int t = lua_gettaintof(L, -1);
        lua_pop(L, 1);
        return t;
    }
};

}  // namespace

TEST_CASE("0x005191c0's switch: which kinds are refused, and when", "[taint]") {
    using R = taint::Rule;
    for (int k : {0, 1, 2, 3, 4, 5, 0x10, 0x11}) CHECK(taint::ruleFor(k) == R::Forbidden);
    for (int k : {6, 7, 8, 9, 0xf, 0x12, 0x13, 0x14, 0x15, 0x17})
        CHECK(taint::ruleFor(k) == R::HardwareEvent);
    for (int k : {0xb, 0xc, 0xd, 0xe, 0x16}) CHECK(taint::ruleFor(k) == R::NotInCombat);
    // 0xa is SendChatMessage's, which the switch does not name.
    for (int k : {0xa, 0x18, 0x40}) CHECK(taint::ruleFor(k) == R::Allowed);
}

TEST_CASE("Secure code is never refused, whatever the state", "[taint]") {
    for (int k = 0; k < 0x18; ++k) {
        CHECK(taint::decide(k, 0, false, true) == taint::Refusal::None);
    }
}

TEST_CASE("Insecure code: refused outright, outside a click, or in combat", "[taint]") {
    using F = taint::Refusal;
    const int t = taint::intern("SomeAddon");
    CHECK(taint::decide(2, t, true, false) == F::Forbidden);
    CHECK(taint::decide(7, t, false, false) == F::Blocked);
    CHECK(taint::decide(7, t, true, true) == F::None);
    CHECK(taint::decide(0xe, t, false, true) == F::BlockedInCombat);
    CHECK(taint::decide(0xe, t, false, false) == F::None);
    CHECK(taint::decide(0xa, t, false, true) == F::None);
}

TEST_CASE("Values an addon makes carry its taint; Blizzard's do not", "[taint]") {
    Fixture f;
    f.run("SecureValue = 1; function SecureFn() return 1 end", 0);
    f.run("AddonValue = 2; Copied = SecureValue", f.addon);
    CHECK(f.globalTaint("SecureValue") == 0);
    CHECK(f.globalTaint("SecureFn") == 0);
    CHECK(f.globalTaint("AddonValue") == f.addon);
    // A secure value written by insecure code is insecure where it lands.
    CHECK(f.globalTaint("Copied") == f.addon);
}

TEST_CASE("Reading a tainted value taints the code that read it", "[taint]") {
    Fixture f;
    f.run("AddonValue = 2", f.addon);
    f.run("before = issecure(); local v = AddonValue; after = issecure(); who = Taint()", 0);
    CHECK(f.globalBool("before"));
    CHECK_FALSE(f.globalBool("after"));
    CHECK(f.globalString("who") == "SomeAddon");
    // Outside every scope nothing is tracked: no entry the client has runs
    // Lua that way, and a host that does must not taint itself by it.
    REQUIRE(luaL_dostring(f.L, "local v = AddonValue; outside = issecure()") == 0);
    CHECK(f.globalBool("outside"));
}

TEST_CASE("A function an addon made taints whoever calls it", "[taint]") {
    Fixture f;
    f.run("function AddonFn() inside = Taint() end", f.addon);
    f.run("AddonFn(); after = Taint()", 0);
    CHECK(f.globalString("inside") == "SomeAddon");
    CHECK(f.globalString("after") == "SomeAddon");
}

TEST_CASE("issecurevariable names who last wrote a variable", "[taint]") {
    Fixture f;
    f.run("SecureValue = 1; t = { field = 1 }", 0);
    f.run("AddonValue = 2; t.addonField = 2", f.addon);
    f.run("a1, a2 = issecurevariable('SecureValue');"
          "b1, b2 = issecurevariable('AddonValue');"
          "c1, c2 = issecurevariable(t, 'addonField');"
          "stillSecure = issecure()", 0);
    CHECK(f.globalBool("a1"));
    CHECK(f.globalString("a2") == "<nil>");
    CHECK_FALSE(f.globalBool("b1"));
    CHECK(f.globalString("b2") == "SomeAddon");
    CHECK(f.globalString("c2") == "SomeAddon");
    // Asking does not taint the asker.
    CHECK(f.globalBool("stillSecure"));
}

TEST_CASE("securecall keeps a callee's taint out of the caller", "[taint]") {
    Fixture f;
    f.run("function AddonFn() return 5 end", f.addon);
    // By name, so the lookup is securecall's own: reading AddonFn here would
    // taint the caller before securecall was reached, as it does in the client.
    f.run("r = securecall('AddonFn'); after = issecure()", 0);
    CHECK(f.globalBool("after"));
    CHECK(f.globalString("r") == "5");
    CHECK(f.globalTaint("r") == 0);
}

TEST_CASE("hooksecurefunc leaves the original secure and the hook insecure", "[taint]") {
    Fixture f;
    f.run("function Original(x) origSecure = issecure(); return x + 1 end", 0);
    f.run("hooksecurefunc('Original', function(x) hookTaint = Taint(); hookArg = x end)",
          f.addon);
    CHECK(f.globalTaint("Original") == 0);
    f.run("res = Original(4); callerSecure = issecure()", 0);
    CHECK(f.globalBool("origSecure"));
    CHECK(f.globalString("hookTaint") == "SomeAddon");
    CHECK(f.globalString("hookArg") == "4");
    CHECK(f.globalString("res") == "5");
    CHECK(f.globalBool("callerSecure"));
}

TEST_CASE("forceinsecure taints the running code", "[taint]") {
    Fixture f;
    f.run("forceinsecure(); who = Taint()", 0);
    CHECK(f.globalString("who") == "*** TaintForced ***");
}

TEST_CASE("Insecure code is refused a protected call, and the interface told", "[taint]") {
    Fixture f;
    f.run("ok = Forbidden()", f.addon);
    CHECK_FALSE(f.globalBool("ok"));
    REQUIRE(f.fired.size() == 1);
    CHECK(f.fired[0].first == "ADDON_ACTION_FORBIDDEN");
    CHECK(f.fired[0].second == std::vector<std::string>{"SomeAddon", "Forbidden()"});

    f.fired.clear();
    f.run("ok = Forbidden()", 0);
    CHECK(f.globalBool("ok"));
    CHECK(f.fired.empty());
}

TEST_CASE("A hardware event lets insecure code through, combat lockdown does not", "[taint]") {
    Fixture f;
    f.run("ok = NeedsClick()", f.addon);
    CHECK_FALSE(f.globalBool("ok"));
    REQUIRE(f.fired.size() == 1);
    CHECK(f.fired[0].first == "ADDON_ACTION_BLOCKED");

    f.fired.clear();
    {
        taint::HardwareEvent click;
        f.run("ok = NeedsClick()", f.addon);
    }
    CHECK(f.globalBool("ok"));
    CHECK_FALSE(taint::hardwareEvent());

    f.run("ok = NotInCombat()", f.addon);
    CHECK(f.globalBool("ok"));
    taint::setCombatLockdown(true);
    f.run("ok = NotInCombat()", f.addon);
    CHECK_FALSE(f.globalBool("ok"));
    REQUIRE(f.fired.size() == 1);
    CHECK(f.fired[0].first == "ADDON_ACTION_BLOCKED");
}

TEST_CASE("A movement call uses the hardware event up", "[taint]") {
    Fixture f;
    taint::HardwareEvent click;
    f.run("ok = Movement()", 0);
    CHECK(f.globalBool("ok"));
    CHECK_FALSE(taint::hardwareEvent());
}

TEST_CASE("/run is the macro taint, and reports as a macro", "[taint]") {
    Fixture f;
    taint::execute(f.L, "ok = Forbidden(); who = Taint()", "test", taint::macro());
    CHECK_FALSE(f.globalBool("ok"));
    CHECK(f.globalString("who").empty());
    REQUIRE(f.fired.size() == 1);
    CHECK(f.fired[0].first == "MACRO_ACTION_FORBIDDEN");
    CHECK(f.fired[0].second == std::vector<std::string>{"Forbidden()"});
    CHECK(taint::current() == 0);
}

TEST_CASE("A handler runs under the taint of whoever set it", "[taint]") {
    Fixture f;
    f.run("function SecureHandler() handlerTaint = Taint() end", 0);
    f.run("function AddonHandler() handlerTaint = Taint() end", f.addon);
    for (const char* name : {"SecureHandler", "AddonHandler"}) {
        {
            taint::Quiet quiet;
            lua_getglobal(f.L, name);
        }
        REQUIRE(taint::pcallHandler(f.L, 0, 0, 0) == 0);
        CHECK(taint::current() == 0);
    }
    CHECK(f.globalString("handlerTaint") == "SomeAddon");
}

TEST_CASE("The bracket puts everything back", "[taint]") {
    Fixture f;
    const lua_TaintState before = *lua_taintstate();
    {
        taint::Scope outer(f.addon, f.addon);
        CHECK(taint::current() == f.addon);
        {
            taint::Scope inner(0);
            CHECK(taint::current() == 0);
        }
        CHECK(taint::current() == f.addon);
    }
    const lua_TaintState after = *lua_taintstate();
    CHECK(after.taint == before.taint);
    CHECK(after.scope == before.scope);
    CHECK(after.objtaint == before.objtaint);
}

// ---- Script slots: the animation system's scripts ---------------------------

namespace {

void registerSlots(lua_State* L) {
    lua_register(L, "SetScriptSlot", wowee::addons::lua_WoweeSetScriptSlot);
    lua_register(L, "GetScriptSlot", wowee::addons::lua_WoweeGetScriptSlot);
    lua_register(L, "HookScriptSlot", wowee::addons::lua_WoweeHookScriptSlot);
    lua_register(L, "RunScriptSlot", wowee::addons::lua_WoweeRunScriptSlot);
}

}  // namespace

TEST_CASE("Animation scripts run one at a time, each under its setter's taint", "[taint]") {
    // The tick that advances every playing group is the client's C
    // (0x0081a2c0 per script); here it is Lua, run secure, and an addon's
    // OnFinished on one group must not taint the Blizzard one after it - nor
    // the tick itself.
    Fixture f;
    registerSlots(f.L);
    f.run("AddonGroup = {} BlizzGroup = {}\n"
          "SetScriptSlot(BlizzGroup, 'OnFinished', function(self)\n"
          "  blizzSelf = (self == BlizzGroup) blizzTaint = Taint() end)", 0);
    f.run("SetScriptSlot(AddonGroup, 'OnFinished', function() addonTaint = Taint() end)",
          f.addon);
    f.run("RunScriptSlot(AddonGroup, 'OnFinished')\n"
          "RunScriptSlot(BlizzGroup, 'OnFinished')\n"
          "tickTaint = Taint()", 0);
    CHECK(f.globalString("addonTaint") == "SomeAddon");
    CHECK(f.globalString("blizzTaint") == "<nil>");
    CHECK(f.globalBool("blizzSelf"));
    CHECK(f.globalString("tickTaint") == "<nil>");
    CHECK(taint::current() == 0);
}

TEST_CASE("Calling an addon's script in place is what tainted the tick", "[taint]") {
    // The shape the animation system had: scripts kept as fields and called
    // from the secure tick. The addon's closure taints the tick for the rest
    // of it, Blizzard's script after it included - which is why the scripts
    // are now run as the client runs them.
    Fixture f;
    f.run("Groups = {}", 0);
    f.run("Groups[1] = { OnFinished = function() end }", f.addon);
    f.run("Groups[2] = { OnFinished = function() after = Taint() end }", 0);
    f.run("for i = 1, 2 do local g = Groups[i] g.OnFinished(g) end", 0);
    CHECK(f.globalString("after") == "SomeAddon");
}

TEST_CASE("Hooking a secure animation script leaves it secure", "[taint]") {
    Fixture f;
    registerSlots(f.L);
    f.run("Group = {}\n"
          "SetScriptSlot(Group, 'OnPlay', function() originalTaint = Taint() end)", 0);
    f.run("HookScriptSlot(Group, 'OnPlay', function() hookTaint = Taint() end)", f.addon);
    f.run("RunScriptSlot(Group, 'OnPlay') afterTaint = Taint()", 0);
    CHECK(f.globalString("originalTaint") == "<nil>");
    CHECK(f.globalString("hookTaint") == "SomeAddon");
    CHECK(f.globalString("afterTaint") == "<nil>");
}

TEST_CASE("Reading an animation script back is reading its setter's value", "[taint]") {
    Fixture f;
    registerSlots(f.L);
    f.run("Group = {}", 0);
    f.run("SetScriptSlot(Group, 'OnStop', function() end)", f.addon);
    f.run("local fn = GetScriptSlot(Group, 'OnStop') readTaint = Taint()", 0);
    CHECK(f.globalString("readTaint") == "SomeAddon");
}

TEST_CASE("A script slot that raises reports and lets the caller go on", "[taint]") {
    Fixture f;
    registerSlots(f.L);
    static int reported = 0;
    reported = 0;
    wowee::addons::setScriptErrorReporter([](lua_State* S, const char*) {
        ++reported;
        lua_pop(S, 1);
    });
    f.run("Group = {}", 0);
    f.run("SetScriptSlot(Group, 'OnLoop', function() error('boom') end)", f.addon);
    f.run("RunScriptSlot(Group, 'OnLoop') reached = true afterTaint = Taint()", 0);
    wowee::addons::setScriptErrorReporter(nullptr);
    CHECK(reported == 1);
    CHECK(f.globalBool("reached"));
    CHECK(f.globalString("afterTaint") == "<nil>");
}

// ---- The host's own bookkeeping ---------------------------------------------

TEST_CASE("A list the host keeps taints nobody when it is kept quietly", "[taint]") {
    // RegisterEvent's listener lists and the OnUpdate list are the client's C
    // structures, kept here in Lua tables every frame's call reaches. An
    // addon's frame appended to one, then read back under a scope - the
    // duplicate check of the next frame to register - tainted that frame's
    // whole OnLoad. Read quietly, it does not.
    Fixture f;
    f.run("Listeners = {}", 0);
    f.run("AddonFrame = {} Listeners[1] = AddonFrame", f.addon);
    {
        taint::Scope secure(0);
        taint::Quiet quiet;
        lua_getglobal(f.L, "Listeners");
        lua_rawgeti(f.L, -1, 1);
        lua_pop(f.L, 2);
    }
    CHECK(taint::current() == 0);
    {
        taint::Scope secure(0);
        lua_getglobal(f.L, "Listeners");
        lua_rawgeti(f.L, -1, 1);
        lua_pop(f.L, 2);
        CHECK(taint::current() == f.addon);   // the read that was the fault
    }
    CHECK(taint::current() == 0);
}
