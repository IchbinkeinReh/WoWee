#include "addons/lua_taint.hpp"

#include <cstring>
#include <unordered_map>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace wowee::addons::taint {

namespace {

// ---- Names ----

std::vector<std::string>& names() {
    // Index 0 is never handed out: 0 is secure.
    static std::vector<std::string> v{std::string()};
    return v;
}

std::unordered_map<std::string, int>& ids() {
    static std::unordered_map<std::string, int> m;
    return m;
}

// The two the client keeps as fixed strings rather than addon names. Made
// first so they cannot collide with an addon that calls itself "".
int gMacro = 0;
int gForced = 0;

void ensureFixed() {
    if (gMacro != 0) return;
    names().push_back("");                     // PTR_DAT_00ac804c
    gMacro = static_cast<int>(names().size()) - 1;
    names().push_back("*** TaintForced ***");  // 0x00a44544
    gForced = static_cast<int>(names().size()) - 1;
}

// ---- The interface's state ----

bool gHardwareEvent = false;   // CGGameUI +0x1250
bool gCombatLockdown = false;  // CGGameUI +0x124c clear
bool gReporting = false;       // DAT_00bd0c68
EventSink gSink;

lua_TaintState& ts() { return *lua_taintstate(); }

// The client's own bracket, kept literally: the running taint changes only
// while some scope is open and the running closure is not an insecure one.
void setIfFree(int taint) {
    lua_TaintState& s = ts();
    if (s.scope != 0 && s.hold == 0) s.taint = taint;
}

void leaveScope() {
    lua_TaintState& s = ts();
    s.scope -= 1;
    if (s.scope < 1) s.scope = 0;
}

int passErrorThrough(lua_State*) { return 1; }

/// The error handler seterrorhandler installed, or nothing to call.
///
/// The client keeps it in a registry reference (DAT_00af576c) and hands it to
/// every pcall it makes for a script. Here seterrorhandler is the
/// interface's own, so the handler is asked for through geterrorhandler.
void pushErrorHandler(lua_State* L) {
    Quiet quiet;
    lua_pushstring(L, "geterrorhandler");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1) && lua_pcall(L, 0, 1, 0) == 0 && lua_isfunction(L, -1))
        return;
    lua_pop(L, 1);
    lua_pushcfunction(L, passErrorThrough);
}

}  // namespace

int intern(const std::string& n) {
    ensureFixed();
    auto it = ids().find(n);
    if (it != ids().end()) return it->second;
    names().push_back(n);
    const int id = static_cast<int>(names().size()) - 1;
    ids().emplace(n, id);
    return id;
}

const char* name(int taint) {
    ensureFixed();
    if (taint <= 0 || taint >= static_cast<int>(names().size())) return nullptr;
    return names()[static_cast<size_t>(taint)].c_str();
}

int macro() { ensureFixed(); return gMacro; }
int forced() { ensureFixed(); return gForced; }

int current() { return ts().taint; }

Scope::Scope(int taint) : saved_(ts().taint), savedObj_(0), setObj_(false) {
    ts().scope += 1;
    setIfFree(taint);
}

Scope::Scope(int taint, int objTaint)
    : saved_(ts().taint), savedObj_(ts().objtaint), setObj_(true) {
    ts().scope += 1;
    setIfFree(taint);
    ts().objtaint = objTaint;
}

Scope::~Scope() {
    setIfFree(saved_);
    leaveScope();
    if (setObj_) ts().objtaint = savedObj_;
}

Quiet::Quiet() : savedTaint_(ts().taint), savedScope_(ts().scope) {
    ts().taint = 0;
    ts().scope = 0;
}

Quiet::~Quiet() {
    ts().taint = savedTaint_;
    ts().scope = savedScope_;
}

void adoptArguments(lua_State* L, int nargs, int record) {
    const int first = lua_gettop(L) - nargs + 1;
    for (int i = first; i <= lua_gettop(L); ++i) {
        if (i == first && lua_istable(L, i)) {
            lua_settaintof(L, i, record);
            continue;
        }
        const int t = lua_gettaintof(L, i);
        if (t == 0) lua_settaintof(L, i, record);
        else setIfFree(t);
    }
}

int pcallHandler(lua_State* L, int nargs, int nresults, int errfunc) {
    const int record = lua_gettaintof(L, lua_gettop(L) - nargs);
    Scope outer(0);       // 0x0048ec10
    Scope inner(record);  // 0x00819ea0
    adoptArguments(L, nargs, record);
    return lua_pcall(L, nargs, nresults, errfunc);
}

void execute(lua_State* L, const char* text, const char* chunkName, int taint) {
    if (!L || !text) return;
    Scope scope(taint);
    const int base = lua_gettop(L);
    pushErrorHandler(L);
    if (luaL_loadbuffer(L, text, std::strlen(text), chunkName ? chunkName : text) == 0)
        lua_pcall(L, 0, 0, base + 1);
    else
        lua_pcall(L, 1, 0, 0);   // the handler, given the compile error
    lua_settop(L, base);
}

void reset() {
    lua_TaintState& s = ts();
    s.taint = 0;
    s.scope = 0;
    s.hold = 0;
    s.objtaint = 0;
    gHardwareEvent = false;
    gCombatLockdown = false;   // 0x00520000 sets +0x124c as the UI is made
    gReporting = false;
}

bool hardwareEvent() { return gHardwareEvent; }
void setHardwareEvent(bool on) { gHardwareEvent = on; }
bool combatLockdown() { return gCombatLockdown; }
void setCombatLockdown(bool locked) { gCombatLockdown = locked; }

Rule ruleFor(int kind) {
    switch (kind) {
    case 0: case 1: case 2: case 3: case 4: case 5:
    case 0x10: case 0x11:
        return Rule::Forbidden;
    case 6: case 7: case 8: case 9:
    case 0xf:
    case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x17:
        return Rule::HardwareEvent;
    case 0xb: case 0xc: case 0xd: case 0xe:
    case 0x16:
        return Rule::NotInCombat;
    default:
        return Rule::Allowed;
    }
}

Refusal decide(int kind, int taint, bool hardware, bool lockdown) {
    if (taint == 0) return Refusal::None;
    switch (ruleFor(kind)) {
    case Rule::Forbidden: return Refusal::Forbidden;
    case Rule::HardwareEvent: return hardware ? Refusal::None : Refusal::Blocked;
    case Rule::NotInCombat: return lockdown ? Refusal::BlockedInCombat : Refusal::None;
    case Rule::Allowed: break;
    }
    return Refusal::None;
}

bool allowed(lua_State* L, int kind) {
    const Refusal r = decide(kind, current(), hardwareEvent(), combatLockdown());
    if (r != Refusal::None) {
        reportRefusal(L, r);
        return false;
    }
    // The movement and binding starts (kind 0) use the hardware event up.
    if (kind == 0) gHardwareEvent = false;
    return true;
}

bool allowedSecureOnly(lua_State* L) {
    if (current() == 0) return true;
    reportRefusal(L, Refusal::Forbidden);
    return false;
}

bool allowedInHardwareEvent(lua_State* L) {
    if (current() == 0 || hardwareEvent()) return true;
    reportRefusal(L, Refusal::Blocked);
    return false;
}

std::string runningFunctionName(lua_State* L) {
    lua_Debug ar;
    std::memset(&ar, 0, sizeof(ar));
    if (!L || !lua_getstack(L, 0, &ar) || !lua_getinfo(L, "n", &ar) || !ar.name)
        return "UNKNOWN()";
    std::string out;
    // 0x00817de0: called as a method, the object's own name in front.
    if (ar.namewhat && std::strcmp(ar.namewhat, "method") == 0) {
        Quiet quiet;
        if (lua_getlocal(L, &ar, 1)) {
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "__name");
                lua_rawget(L, -2);
                const char* objName = lua_tostring(L, -1);
                out = objName && *objName ? objName : "<unnamed>";
                out += ":";
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    out += ar.name;
    out += "()";
    return out;
}

void reportRefusal(lua_State* L, Refusal how) {
    if (how == Refusal::None || gReporting) return;
    gReporting = true;
    const std::string fn = runningFunctionName(L);
    const int t = current();
    const bool forbidden = how == Refusal::Forbidden;
    if (gSink) {
        if (t == macro()) {
            gSink(forbidden ? "MACRO_ACTION_FORBIDDEN" : "MACRO_ACTION_BLOCKED", {fn});
        } else {
            const char* who = name(t);
            gSink(forbidden ? "ADDON_ACTION_FORBIDDEN" : "ADDON_ACTION_BLOCKED",
                  {who ? who : "", fn});
        }
    }
    gReporting = false;
}

void setEventSink(EventSink sink) { gSink = std::move(sink); }

// ---- The Lua API (0x00816da0-0x00817170) ----

namespace {

// issecure() (0x00816da0)
int lua_issecure(lua_State* L) {
    if (current() == 0) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

// issecurevariable([table,] "name") (0x00816de0) -> 1, nil or nil, "taint"
int lua_issecurevariable(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_insert(L, 1);
    }
    if (!lua_isstring(L, 2))
        return luaL_error(L, "Usage: issecurevariable([table,] \"variable\")");
    const char* key = lua_tostring(L, 2);
    int found = 0;
    {
        Scope scope(0);
        lua_pushstring(L, key);
        lua_gettable(L, 1);                   // through __index, as 0x0084e560
        found = current();
        lua_settop(L, -3);
    }
    if (found == 0) {
        lua_pushnumber(L, 1);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        lua_pushstring(L, name(found));
    }
    return 2;
}

// forceinsecure() (0x00816f00)
int lua_forceinsecure(lua_State*) {
    lua_TaintState& s = ts();
    if (s.taint == 0 && s.scope != 0 && s.hold == 0) s.taint = forced();
    return 0;
}

// securecall(func or "name", ...) (0x00816f30): what it returns carries the
// caller's taint, and nothing the call picked up is kept.
int lua_securecall(lua_State* L) {
    const int saved = current();
    int n = 0;
    {
        ts().scope += 1;
        if (lua_isstring(L, 1)) {
            lua_pushstring(L, lua_tostring(L, 1));
            lua_rawget(L, LUA_GLOBALSINDEX);
            lua_remove(L, 1);
            lua_insert(L, 1);
        }
        if (lua_gettop(L) == 0) lua_pushnil(L);
        pushErrorHandler(L);
        lua_insert(L, 1);
        if (lua_pcall(L, lua_gettop(L) - 2, LUA_MULTRET, 1) == 0) lua_remove(L, 1);
        else lua_settop(L, -3);
        n = lua_gettop(L);
        for (int i = 1; i <= n; ++i) lua_settaintof(L, i, saved);
        setIfFree(saved);
        leaveScope();
    }
    return n;
}

// hooksecurefunc([table,] "name", hook) (0x00817170): the hook runs after
// the original with the original's arguments, under the taint of whoever
// hooked it, and nothing it does is kept - the original's own taint is what
// the caller sees, so hooking a secure function leaves it secure.
int lua_hooksecurefunc(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_insert(L, 1);
    }
    if (!lua_isstring(L, 2) || lua_type(L, 3) != LUA_TFUNCTION)
        return luaL_error(L, "Usage: hooksecurefunc([table,] \"function\", hookfunc)");
    lua_settop(L, 3);                         // table, name, hook
    const int caller = current();
    ts().scope += 1;
    setIfFree(0);
    lua_pushstring(L, lua_tostring(L, 2));    // a new string, as 0x0084e350
    lua_gettable(L, 1);                       // table, name, hook, original
    const bool hooked = lua_type(L, -1) == LUA_TFUNCTION;
    if (hooked) {
        const int original = current();
        ts().scope += 1;
        lua_insert(L, 3);                     // table, name, original, hook
        lua_settaintof(L, 3, original);
        lua_settaintof(L, 4, caller);
        lua_pushcclosure(L, lua_SecureHookWrapper, 2);
        lua_settaintof(L, 3, original);
        setIfFree(original);
        leaveScope();
        lua_rawset(L, 1);                     // table[name] = wrapper
    }
    setIfFree(caller);
    leaveScope();
    if (!hooked)
        return luaL_error(L, "hooksecurefunc(): %s is not a function", lua_tostring(L, 2));
    lua_settop(L, 0);
    return 0;
}

}  // namespace

int lua_SecureHookWrapper(lua_State* L) {
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) lua_pushvalue(L, i);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, n + 1);
    lua_call(L, n, LUA_MULTRET);              // the original, errors and all
    const int results = lua_gettop(L) - n;
    for (int i = 0; i < results; ++i) lua_insert(L, 1);
    const int saved = current();
    ts().scope += 1;
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, results + 1);
    pushErrorHandler(L);
    lua_insert(L, results + 1);
    if (lua_pcall(L, n, 0, results + 1) == 0) lua_settop(L, -2);
    else lua_settop(L, -3);
    setIfFree(saved);
    leaveScope();
    return results;
}

void registerLuaApi(lua_State* L) {
    lua_register(L, "issecure", lua_issecure);
    lua_register(L, "issecurevariable", lua_issecurevariable);
    lua_register(L, "forceinsecure", lua_forceinsecure);
    lua_register(L, "securecall", lua_securecall);
    lua_register(L, "hooksecurefunc", lua_hooksecurefunc);
}

}  // namespace wowee::addons::taint
