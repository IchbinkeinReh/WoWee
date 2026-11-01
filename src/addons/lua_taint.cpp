#include "addons/lua_taint.hpp"

#include <cstdio>
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

// The handler seterrorhandler installed, kept as a reference in the registry
// and nothing else - the client holds the same thing in DAT_00af5770, and
// geterrorhandler (0x00817420) is a rawgeti of that one number. The state the
// reference belongs to is part of the pair: a client that tears the interface
// down and builds it again builds a new lua_State in which that number names
// something else or nothing at all.
lua_State* gHandlerState = nullptr;
int gHandlerRef = LUA_NOREF;

// Written 1 on the line before the client calls that handler and back to 0 on
// the line after it (0x00819730, DAT_00d3f79c - set and cleared in place, with
// nothing else in the binary touching it). debuglocals reads it and answers
// nothing outside that window: the frame whose locals it would name belongs to
// a call that has already unwound.
bool gInErrorHandler = false;

}  // namespace

int scriptErrorHandler(lua_State* L) {
    // 0x00819730, the one C function the client refers to once when the state
    // is made (DAT_00af576c) and hands to every script call it protects. A
    // message that is not a string - a script that error()ed a table or a
    // number - is said as "UNKNOWN ERROR"; then whatever handler
    // seterrorhandler installed runs over the message, and what it returns is
    // the message the dispatch reports.
    //
    // FrameXML installs that handler itself (basiccontrols.xml:
    // _ERRORMESSAGE), and that is where a script error becomes something on
    // the screen: under the scriptErrors cvar it puts the text in
    // BasicScriptErrors and counts them, and at the thousandth it shows
    // TOO_MANY_LUA_ERRORS instead. The cvar is registered "0" (0x0051dd63), so
    // none of that is on until the interface is asked for errors - but nothing
    // here reached the handler at all, so asking changed nothing: the message
    // went to the log and nowhere else.
    //
    // The client calls its handler unprotected, from inside Lua's own error
    // handling; one that breaks there leaves Lua's "error in error handling"
    // behind, which costs the destructors of the taint brackets around the
    // script that broke. A pcall is given here instead: a handler that breaks
    // says so as its own error, and those brackets still close on the way out.
    if (!lua_isstring(L, -1)) {
        lua_pushstring(L, "UNKNOWN ERROR");
        lua_replace(L, -2);
    }
    if (gHandlerState != L || gHandlerRef == LUA_NOREF) return 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, gHandlerRef);
    lua_insert(L, -2);            // the handler goes under the message
    gInErrorHandler = true;       // the window debuglocals answers in
    lua_pcall(L, 1, 1, 0);        // the handler's return takes its place
    gInErrorHandler = false;
    return 1;
}

namespace {

// seterrorhandler(errfunc) (0x008173c0): the reference is dropped and the new
// one taken, and only a function is accepted - the client's own complaint
// otherwise, word for word.
int lua_seterrorhandler(lua_State* L) {
    if (lua_type(L, 1) != LUA_TFUNCTION)
        return luaL_error(L, "Usage: seterrorhandler(errfunc)");
    Quiet quiet;                  // luaL_ref neither taints nor is tainted
    if (gHandlerState == L && gHandlerRef != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, gHandlerRef);
    lua_pushvalue(L, 1);
    gHandlerState = L;
    gHandlerRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

// geterrorhandler() (0x00817420) - nil while nobody has installed one, as it
// is until the interface loads.
int lua_geterrorhandler(lua_State* L) {
    if (gHandlerState == L && gHandlerRef != LUA_NOREF)
        lua_rawgeti(L, LUA_REGISTRYINDEX, gHandlerRef);
    else
        lua_pushnil(L);
    return 1;
}

// debugstack([thread,] start [, count1 [, count2]]) (0x008178f0).
//
// The other half of what the error handler shows: Blizzard_DebugTools'
// ScriptErrorsFrame_OnError does `local stack = debugstack(DEBUGLOCALS_LEVEL)`
// and puts the result on the frame's "Stack:" line, and here that answered an
// empty string, so the frame said what broke and nothing about where it was
// reached from. Written from the same shape as lua_GenTraces - Lua's own
// luaG_traceback, which is where the client's three wording strings come from:
//
//     <short_src>:[<line>:] <what it is>\n
//
// with the tail naming a named function (`in function \`Foo\''), a chunk
// (in main chunk), a C or tail frame (` ?', which is what makes the familiar
// `[C]: ?` line), or an anonymous one by where it was defined
// (in function <src:linedefined>). start is 1 measured from the caller unless
// a thread names the state, in which case it is 0; count1 is 12 levels and
// count2 the 10 levels still worth showing after the ellipsis - and the
// ellipsis is skipped entirely when what is left is short enough that hiding it
// would hide more than it saves.
int lua_debugstack(lua_State* L) {
    lua_State* target = L;
    int start = 1, count1 = 12, count2 = 10;
    if (lua_type(L, 1) == LUA_TTHREAD) {
        target = lua_tothread(L, 1);
        lua_remove(L, 1);
        start = 0;
    }
    // Each number at index 1 in turn, consumed from the front the way the
    // client re-reads that one index: the first value that is not a number
    // stays, because from there on it is the prefix instead of a count.
    int* const counts[] = {&start, &count1, &count2};
    for (int* count : counts) {
        if (!lua_isnumber(L, 1)) break;
        *count = static_cast<int>(lua_tointeger(L, 1));
        lua_remove(L, 1);
    }
    // Anything left is a prefix the client concatenates in front of the trace;
    // a value that is not one is handed straight back. The empty seed is what
    // a trace with no levels at all returns.
    if (lua_gettop(L) == 0) lua_pushliteral(L, "");
    else if (!lua_isstring(L, 1)) return 1;

    lua_Debug ar;
    if (!lua_getstack(target, start, &ar)) { lua_concat(L, lua_gettop(L)); return 1; }
    int level = start;
    const int cutoff = start + count1;
    bool mayEllipsis = true;
    do {
        int next = level + 1;
        if (cutoff < next && mayEllipsis) {
            mayEllipsis = false;
            if (lua_getstack(target, next + count2, &ar)) {
                lua_pushliteral(L, "...\n");
                while (lua_getstack(target, next + 1 + count2, &ar)) ++next;
            } else {
                next = level;    // barely past the cut: show them all instead
            }
        } else if (lua_getinfo(target, "Sn", &ar)) {
            std::string line(ar.short_src);
            line += ":";
            if (ar.currentline > 0) line += std::to_string(ar.currentline) + ":";
            const char namewhat = ar.namewhat[0];
            const char what = ar.what[0];
            if (namewhat == 'f' || namewhat == 'g' ||
                namewhat == 'l' || namewhat == 'm')
                line += std::string(" in function `") + (ar.name ? ar.name : "") + "'";
            else if (what == 'm') line += " in main chunk";
            else if (what == 'C' || what == 't') line += " ?";
            else
                line += " in function <" + std::string(ar.source) + ":" +
                        std::to_string(ar.linedefined) + ">";
            line += "\n";
            lua_pushstring(L, line.c_str());
        }
        level = next;
    } while (lua_getstack(target, level, &ar));
    lua_concat(L, lua_gettop(L));
    return 1;
}

// How wide the client's buffer for one debuglocals answer is: 0x1000, a stack
// buffer memset to zero at 0x00819a60 and appended to line by line, so a
// function with more to say than that gets the front of the list.
constexpr size_t kLocalsRoom = 0x1000;

// One line of that answer, for the value sitting at the top of the stack
// (0x00819300, which neither pops the value nor counts it). `depth` is the
// indent: 0 for a local or an upvalue of the frame being reported, 1 for an
// entry of a table that is one.
void localLine(lua_State* L, const char* name, int depth, std::string& out) {
    const std::string pad(depth, ' ');
    const int type = lua_type(L, -1);
    const char* text = nullptr;
    switch (type) {
    case LUA_TNIL:
        out += pad + name + " = nil\n";
        return;
    case LUA_TBOOLEAN:
        out += pad + name + (lua_toboolean(L, -1) ? " = true\n" : " = false\n");
        return;
    case LUA_TLIGHTUSERDATA:
    case LUA_TUSERDATA:
        out += pad + name + " = <userdata>\n";
        return;
    case LUA_TNUMBER:
    case LUA_TSTRING:
        // lua_tostring says a number in this Lua - it converts the slot in
        // place, which is how a value gets Lua's own "%.14g". The quotes are
        // what tells a string from a number, and they go on the string.
        text = lua_tostring(L, -1);
        out += pad + name + " = ";
        if (type == LUA_TSTRING)
            out += std::string("\"") + (text ? text : "") + "\"\n";
        else
            out += std::string(text ? text : "") + "\n";
        return;
    case LUA_TTABLE: {
        // The client reads t[0] first and, when that holds a lightuserdata,
        // prints what it points at in place of "<table>" - a way its own
        // tables carry a size. Key 0 is just a key in this Lua (luaH_getnum),
        // and nothing here puts a lightuserdata there, so every table is a
        // plain one.
        out += pad + name + " = <table> {\n";
        if (depth == 0) {
            // Only a top-level table is walked, and only one level down: the
            // recursive call is made at indent 1, where a table prints its
            // own braces and nothing between them.
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                char key[32];
                if (lua_isnumber(L, -2)) {
                    std::snprintf(key, sizeof key, "%d",
                                  static_cast<int>(lua_tonumber(L, -2)));
                    localLine(L, key, 1, out);
                } else {
                    // A key that is not a number is said the way a value is;
                    // only one with no wording at all - a function, say -
                    // falls back on its type name.
                    text = lua_tostring(L, -2);
                    localLine(L, text ? text
                                      : lua_typename(L, lua_type(L, -2)), 1, out);
                }
                lua_pop(L, 1);        // the value; the key is the cursor
            }
        }
        out += pad + "}\n";
        return;
    }
    case LUA_TFUNCTION: {
        // ">nS": the copy pushed here is the one getinfo reads and drops, so
        // the value stays for the caller to pop. 'n' gives a name only for a
        // function found by walking the stack, so in practice every line this
        // produces is the unnamed one - which is the branch the client's own
        // wording spends more room on, and both are kept.
        lua_pushvalue(L, -1);
        lua_Debug ar;
        if (lua_getinfo(L, ">nS", &ar)) {
            const std::string where = std::string(ar.source) + ":" +
                                      std::to_string(ar.linedefined);
            if (ar.name)
                out += pad + name + " = " + ar.name + "() defined " + where + "\n";
            else
                out += pad + name + " = <function> defined " + where + "\n";
        } else {
            out += pad + name + " = <function>\n";
        }
        return;
    }
    default:
        out += pad + name + " = <" + lua_typename(L, type) + ">\n";
        return;
    }
}

// debuglocals([level]) (0x00819a60): the "Locals:" line of the same error
// frame, one line per local and then per upvalue of one frame, with a
// top-level table's entries listed under it. The level is counted the way
// debugstack counts it - 1 being the frame above the one that asked - and the
// client clamps anything below that to 1 rather than answering nothing.
int lua_debuglocals(lua_State* L) {
    if (!gInErrorHandler) return 0;    // nothing at all, as the client answers
    double wanted = 1;
    if (lua_isnumber(L, 1)) wanted = lua_tonumber(L, 1);
    // Rounded, not cut off: the client turns the number into an int through
    // the rounding conversion, so 3.5 names the fourth frame and not the
    // third.
    int level = static_cast<int>(wanted + (wanted < 0 ? -0.5 : 0.5));
    if (level < 1) level = 1;
    if (!lua_checkstack(L, LUA_MINSTACK)) return 0;
    std::string out;
    lua_Debug ar;
    if (lua_getstack(L, level, &ar)) {
        const char* name;
        for (int i = 1; (name = lua_getlocal(L, &ar, i)) != nullptr; ++i) {
            localLine(L, name, 0, out);
            lua_pop(L, 1);
        }
        // "f" over a frame's record pushes that frame's function, which is
        // what its upvalues are read from; then it comes back off.
        lua_getinfo(L, "f", &ar);
        for (int i = 1; (name = lua_getupvalue(L, -1, i)) != nullptr; ++i) {
            localLine(L, name, 0, out);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (out.size() > kLocalsRoom) out.resize(kLocalsRoom);
    lua_pushstring(L, out.c_str());
    return 1;
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
    return callProtected(L, nargs, nresults, errfunc);
}

int callProtected(lua_State* L, int nargs, int nresults, int errfunc) {
    // A caller that names no message handler gets the client's own: 0x00819ea0
    // puts DAT_00af576c - 0x00819730, held as a reference - under the function
    // and calls with -(nargs + 2) as its errfunc, so the handler runs while the
    // brackets above are still open and sees the stack that broke. The slot
    // comes back off afterwards: lua_pcall leaves the handler where it was for
    // the next call, and every caller of this one counts the stack around its
    // own.
    if (errfunc != 0) return lua_pcall(L, nargs, nresults, errfunc);
    // Under the function, not on top: lua_pcall calls whatever sits nargs + 1
    // below the top, so a handler pushed last turned the call into one of the
    // first argument with the rest and the handler as its arguments.
    const int slot = lua_gettop(L) - nargs;
    lua_pushcfunction(L, scriptErrorHandler);
    lua_insert(L, slot);
    const int rc = lua_pcall(L, nargs, nresults, slot);
    lua_remove(L, slot);
    return rc;
}

void execute(lua_State* L, const char* text, const char* chunkName, int taint) {
    if (!L || !text) return;
    Scope scope(taint);
    const int base = lua_gettop(L);
    lua_pushcfunction(L, scriptErrorHandler);
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
        lua_pushcfunction(L, scriptErrorHandler);
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
    lua_pushcfunction(L, scriptErrorHandler);
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
    // Registered here rather than defined in the bootstrap chunk because the
    // client's pair (the entries at 0x00a44458 and 0x00a44460, 0x008173c0 and
    // 0x00817420) is what holds the handler, as a reference: the handler this
    // file hands to every script call is the one they store.
    lua_register(L, "seterrorhandler", lua_seterrorhandler);
    lua_register(L, "geterrorhandler", lua_geterrorhandler);
    // Two entries further along the same table (0x00a44480 and 0x00a44488) are
    // debugstack and debuglocals, and a script error frame reads both off the
    // error it was given - the first for its "Stack:" line, the second for
    // "Locals:". A Lua pair of shims could give either of them an answer; only
    // C can see the frames and the values they hold.
    lua_register(L, "debugstack", lua_debugstack);
    lua_register(L, "debuglocals", lua_debuglocals);
}

}  // namespace wowee::addons::taint
