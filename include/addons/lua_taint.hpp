#pragma once

// The client's add-on protection: who the running code belongs to, and what it
// may therefore do.
//
// wow.exe 12340 keeps the answer in DAT_00d4139c - the name of the addon whose
// code is running, the empty string at PTR_DAT_00ac804c for /run and
// RunScript (0x00510b30), and 0 for Blizzard's own code. Its Lua is patched to
// carry the same thing on every value and every object (see lua_TaintState in
// extern/lua-5.1.5/src/lua.h): a value insecure code produces is tainted with
// its name, and secure code that reads one becomes insecure itself for the rest
// of the call. That is how an addon replacing a global function makes the
// Blizzard code that calls it insecure, without the replacement ever being
// asked about.
//
// Protected functions ask 0x005191c0 before they act. Tainted, some are refused
// outright, some outside a hardware event (a click, an Enter, a key binding),
// and some during combat lockdown - see ruleFor().
//
// Dependent on nothing but Lua, so the rules and the state machine can be
// tested without a window: see tests/test_lua_taint.cpp.

#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace wowee::addons::taint {

// ---- Names ------------------------------------------------------------------
//
// A taint is a number in the Lua core and a name everywhere it is shown. 0 is
// secure; every other number is one of the names below, interned once and kept
// for the life of the process, as the client keeps the addon's name string.

/// The taint for `name`, made if it is new. Never 0.
int intern(const std::string& name);
/// The name a taint stands for: "" for /run, nullptr for secure (0).
const char* name(int taint);
/// /run, /script and RunScript (PTR_DAT_00ac804c, the empty string).
int macro();
/// What forceinsecure() taints the running code with (0x00816f00).
int forced();

// ---- The running code ---------------------------------------------------------

/// DAT_00d4139c: what the running code is tainted by, 0 when it is secure.
int current();

/// The bracket every C entry into Lua puts around it (0x0048ec10, 0x005f80b0,
/// 0x0081ac90, ...): open a scope in which reading a tainted value taints the
/// running code, and run tainted by `taint` - unless the running closure is
/// itself insecure, whose taint is held. Put back as found on the way out.
///
/// `objTaint` is DAT_00d413ac, what new objects are tainted by while the code
/// is insecure: an addon's own name while its files load (0x005f80b0), the
/// file's while a template it defined is inherited (0x004932c0).
class Scope {
public:
    explicit Scope(int taint);
    Scope(int taint, int objTaint);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int saved_;
    int savedObj_;
    bool setObj_;
};

/// Bookkeeping that must neither taint nor be tainted: the running taint is 0
/// and reading a value taints nothing until this ends, as luaH_resize
/// (0x0085c6f0) and luaL_ref (0x0084f6c0) do it.
///
/// For the host's own reads of handler tables. The client keeps a script as a
/// reference in C and never reads it through Lua; this client keeps it in a
/// table, and fetching it under a scope would taint whoever is running with
/// the handler's own taint before it had been called at all.
class Quiet {
public:
    Quiet();
    ~Quiet();
    Quiet(const Quiet&) = delete;
    Quiet& operator=(const Quiet&) = delete;

private:
    int savedTaint_;
    int savedScope_;
};

/// Run a script handler the way 0x0048ec10 and 0x00819ea0 do: secure to begin
/// with, then tainted by whoever set the handler, which is the taint the
/// function value carries. Same stack contract as lua_pcall.
int pcallHandler(lua_State* L, int nargs, int nresults, int errfunc);

/// The handler's arguments as 0x00819ea0 hands them over, once the running
/// taint is the handler's (`record`): `self` fresh out of the frame's registry
/// reference, so it carries the handler's taint and not whoever stored the
/// frame; the rest copied, so a secure one takes the handler's taint and an
/// insecure one taints the call.
void adoptArguments(lua_State* L, int nargs, int record);

/// Compile and run `text` tainted by `taint`, as FrameScript__Execute
/// (0x00819210) does for RunScript and /run: a script that will not compile
/// and one that raises both go to the error handler, and neither raises
/// further. `chunkName` is what the error names it by - the client uses the
/// text itself.
void execute(lua_State* L, const char* text, const char* chunkName, int taint);

/// Back to a clean state, for a Lua state that is about to be made anew.
void reset();

// ---- The interface's state the checks read -------------------------------------

/// CGGameUI +0x1250: inside a hardware event - a click on a button (0x0096fa80,
/// 0x0096fb60), Enter in an edit box (0x00967760), a key binding (0x00563150).
bool hardwareEvent();
void setHardwareEvent(bool on);

/// Sets the hardware event for its lifetime, and clears it after, as each of
/// the client's call sites does (they clear rather than restore).
class HardwareEvent {
public:
    HardwareEvent() { setHardwareEvent(true); }
    ~HardwareEvent() { setHardwareEvent(false); }
    HardwareEvent(const HardwareEvent&) = delete;
    HardwareEvent& operator=(const HardwareEvent&) = delete;
};

/// CGGameUI +0x124c clear: combat lockdown (InCombatLockdown, 0x00511cc0).
bool combatLockdown();
void setCombatLockdown(bool locked);

// ---- The check ------------------------------------------------------------------

/// What 0x005191c0 does with a kind when the running code is tainted.
enum class Rule {
    Allowed,         // not a kind the switch names
    Forbidden,       // 0-5, 0x10, 0x11: refused outright
    HardwareEvent,   // 6-9, 0xf, 0x12-0x15, 0x17: refused outside one
    NotInCombat,     // 0xb-0xe, 0x16: refused in combat lockdown
};
Rule ruleFor(int kind);

/// 0x00513530's second argument: how a refusal is reported.
enum class Refusal {
    None = -1,
    Forbidden = 0,       // ADDON_ACTION_FORBIDDEN / MACRO_ACTION_FORBIDDEN
    Blocked = 1,         // ADDON_ACTION_BLOCKED / MACRO_ACTION_BLOCKED
    BlockedInCombat = 2, // the same events; only the taint log tells them apart
};

/// The decision alone: what 0x005191c0 refuses with, given the state.
Refusal decide(int kind, int taint, bool hardwareEvent, bool combatLockdown);

/// 0x005191c0: may the running code do a protected thing of this kind? False
/// when refused, which is reported first; a kind 0 that goes ahead uses up the
/// hardware event.
bool allowed(lua_State* L, int kind);

/// The callers that ask DAT_00d4139c themselves rather than through
/// 0x005191c0. Insecure, the first refuses outright (CGGameUI__Target
/// 0x00524bf0, TargetNearest* 0x00524fc0, InteractUnit 0x00527f00, the focus
/// at 0x0051ff20); the second outside a hardware event (UninviteUnit
/// 0x0051a7a0, SetCurrentTitle 0x00522e30). Both report as 0x005191c0 does.
bool allowedSecureOnly(lua_State* L);
bool allowedInHardwareEvent(lua_State* L);

/// 0x00513530: report a refusal for the function that is running - the event
/// for the running taint, with the function's name. Once at a time.
void reportRefusal(lua_State* L, Refusal how);

/// What the name of the running function reads as in those events: "Name()",
/// "FrameName:Method()" for a method on a frame (0x00817ee0, 0x00817de0).
std::string runningFunctionName(lua_State* L);

/// Where the events go: the interface's event dispatch, set by the engine.
using EventSink = std::function<void(const std::string& event,
                                     const std::vector<std::string>& args)>;
void setEventSink(EventSink sink);

/// issecure, issecurevariable, forceinsecure, securecall and hooksecurefunc
/// (0x00816da0-0x00817170), registered into a state's globals.
void registerLuaApi(lua_State* L);

/// The closure hooksecurefunc and HookScript put in a function's place
/// (0x00817050): upvalue 1 the original, upvalue 2 the hook.
int lua_SecureHookWrapper(lua_State* L);

}  // namespace wowee::addons::taint
