#include "addons/lua_handler_globals.hpp"
#include "addons/lua_taint.hpp"

#include <cstring>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace wowee::addons {

namespace {

bool gLegacyHandlerGlobals = false;

/// How deep the dispatch is, so a nested handler saves into its own row.
///
/// Handlers nest: Show() runs OnShow from inside whoever called it. Without the
/// save the outer handler's arg1 would be the inner one's from the moment it
/// returned.
int gHandlerDepth = 0;
int gHandlerSaveRef = LUA_NOREF;

constexpr int kMaxHandlerArgs = 9;
const char* const kHandlerArgNames[kMaxHandlerArgs] = {
    "arg1", "arg2", "arg3", "arg4", "arg5", "arg6", "arg7", "arg8", "arg9"};
/// `this`, `event`, and one per argN.
constexpr int kHandlerSaveSlots = 2 + kMaxHandlerArgs;

/// Read a global without the missing-API fallback answering for it.
///
/// lua_getglobal goes through _G's __index, which with that fallback installed
/// hands back a callable stand-in for a name nothing has defined - and what is
/// wanted here is the value to put back afterwards, which for an undefined name
/// is nothing at all.
void rawGetGlobal(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
}

/// Push the table the saved values live in.
///
/// Held by reference rather than looked up by name, and checked rather than
/// trusted: a reference belongs to one lua_State and this file outlives them.
/// A client that tears the interface down and builds it again gets a new state
/// in which that number means something else or nothing at all, and writing
/// through it is not an error Lua reports - it is a write into whatever is
/// there.
void pushSaveTable(lua_State* L) {
    if (gHandlerSaveRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, gHandlerSaveRef);
        if (lua_istable(L, -1)) return;
        lua_pop(L, 1);
    }
    lua_newtable(L);
    lua_pushvalue(L, -1);
    gHandlerSaveRef = luaL_ref(L, LUA_REGISTRYINDEX);
    gHandlerDepth = 0;
}

}  // namespace

void setLegacyHandlerGlobals(bool on) { gLegacyHandlerGlobals = on; }
bool legacyHandlerGlobals() { return gLegacyHandlerGlobals; }

int pcallScript(lua_State* L, const char* script, int nargs, int errfunc) {
    // A handler runs secure to begin with (0x0048ec10) and then tainted by
    // whoever set it (0x00819ea0) - which is the taint its function value
    // carries, SetScript having given it the setter's.
    if (!gLegacyHandlerGlobals || nargs < 1)
        return taint::pcallHandler(L, nargs, 0, errfunc);
    const int record = lua_gettaintof(L, lua_gettop(L) - nargs);
    taint::Scope outer(0);

    const int selfIdx = lua_gettop(L) - nargs + 1;
    // OnEvent alone puts the event name before the payload; every other
    // handler's own arguments start at arg1.
    //
    // And only if it was given one. Every dispatch of an OnEvent here passes
    // the name, but reading a stack slot that was never pushed is not an error
    // Lua reports - it hands back whatever is there - so the count decides
    // rather than the handler's name alone.
    const bool isEvent = script && std::strcmp(script, "OnEvent") == 0 && nargs >= 2;
    const int firstArg = selfIdx + (isEvent ? 2 : 1);
    int argCount = lua_gettop(L) - firstArg + 1;
    if (argCount < 0) argCount = 0;
    if (argCount > kMaxHandlerArgs) argCount = kMaxHandlerArgs;

    // Only the names about to be overwritten are saved, and only those are put
    // back. A client before 3.0 left the higher argN wherever the last event
    // had left them, and a body written against one reads no further than the
    // count its own event carries.
    // The globals themselves are set secure and taint nothing, as 0x00819ea0
    // sets them with the scope closed.
    int rc = 0;
    const int base = gHandlerDepth * kHandlerSaveSlots;
    {
    taint::Quiet quiet;
    pushSaveTable(L);
    const int save = lua_gettop(L);
    rawGetGlobal(L, "this");
    lua_rawseti(L, save, base + 1);
    if (isEvent) {
        rawGetGlobal(L, "event");
        lua_rawseti(L, save, base + 2);
    }
    for (int i = 0; i < argCount; ++i) {
        rawGetGlobal(L, kHandlerArgNames[i]);
        lua_rawseti(L, save, base + 3 + i);
    }
    lua_pop(L, 1);   // the save table - the call's own stack is left as it was

    lua_pushvalue(L, selfIdx);
    lua_setglobal(L, "this");
    if (isEvent) {
        lua_pushvalue(L, selfIdx + 1);
        lua_setglobal(L, "event");
    }
    for (int i = 0; i < argCount; ++i) {
        lua_pushvalue(L, firstArg + i);
        lua_setglobal(L, kHandlerArgNames[i]);
    }

    }

    ++gHandlerDepth;
    {
        taint::Scope inner(record);
        taint::adoptArguments(L, nargs, record);
        rc = lua_pcall(L, nargs, 0, errfunc);
    }
    --gHandlerDepth;

    // Above whatever pcall left - nothing on success, the error on failure -
    // and popped again, so a caller still finds its error at the top.
    taint::Quiet quiet;
    pushSaveTable(L);
    const int back = lua_gettop(L);
    lua_rawgeti(L, back, base + 1);
    lua_setglobal(L, "this");
    if (isEvent) {
        lua_rawgeti(L, back, base + 2);
        lua_setglobal(L, "event");
    }
    for (int i = 0; i < argCount; ++i) {
        lua_rawgeti(L, back, base + 3 + i);
        lua_setglobal(L, kHandlerArgNames[i]);
    }
    lua_pop(L, 1);
    return rc;
}

int lua_WoweeFireOnLoad(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    {
        // Fetched the way the client holds it, as a reference: reading the
        // handler out of the table must not taint the file being loaded.
        taint::Quiet quiet;
        lua_getfield(L, 1, "__scripts");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
        lua_getfield(L, -1, "OnLoad");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return 0; }
        lua_remove(L, -2);                // __scripts, leaving the handler
    }
    lua_pushvalue(L, 1);                  // self
    if (!gLegacyHandlerGlobals) {
        // 0x00493230 runs it through 0x0048ec10 like any other script. A
        // pcall so the taint is put back before a raise goes on up - it is
        // raised again, because an OnLoad that raises has always taken the
        // rest of its file with it.
        if (taint::pcallHandler(L, 1, 0, 0) != 0) return lua_error(L);
        return 0;
    }
    // The saved value rides on the stack below the call rather than anywhere it
    // would have to be cleaned up, because a raise here does not return.
    {
        taint::Quiet quiet;
        rawGetGlobal(L, "this");
        lua_insert(L, -3);                // beneath the handler and its argument
        lua_pushvalue(L, 1);
        lua_setglobal(L, "this");
    }
    if (taint::pcallHandler(L, 1, 0, 0) != 0) {
        {
            taint::Quiet quiet;
            lua_pushvalue(L, -2);
            lua_setglobal(L, "this");
        }
        return lua_error(L);
    }
    taint::Quiet quiet;
    lua_setglobal(L, "this");             // what was saved, still on the stack
    return 0;
}

}  // namespace wowee::addons
