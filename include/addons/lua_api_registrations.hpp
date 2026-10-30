// lua_api_registrations.hpp - Forward declarations for per-domain Lua API
// registration functions.  Called from LuaEngine::registerCoreAPI().
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#pragma once

#include <string>
#include <vector>

struct lua_State;

namespace wowee::addons {

void registerUnitLuaAPI(lua_State* L);
void registerSpellLuaAPI(lua_State* L);
void registerInventoryLuaAPI(lua_State* L);
void registerQuestLuaAPI(lua_State* L);
void registerLfgLuaAPI(lua_State* L);
void registerSocketLuaAPI(lua_State* L);
void registerSocialLuaAPI(lua_State* L);
void registerSystemLuaAPI(lua_State* L);
/// The answers from subsystems this client does not have (lua_absent_api).
void registerAbsentLuaAPI(lua_State* L);
/// What is asked of an item or a spell without using it (lua_item_query_api).
void registerItemQueryLuaAPI(lua_State* L);
/// The arena commentator's functions (lua_commentator_api).
void registerCommentatorLuaAPI(lua_State* L);
/// CloseTabardCreation (lua_tabard_api).
void registerTabardLuaAPI(lua_State* L);
/// The TabardModel frame's methods onto the table on top of the stack - the
/// metatable every frame shares, so applied with the frame methods.
void addTabardModelMethods(lua_State* L);

/// Re-apply every CVar restored from disk, now that the services behind them
/// exist. See applyCVarSideEffects.
void applyStoredCVarSideEffects(lua_State* L);

/// Re-apply the sound switches from the cvar store.
///
/// The settings panel owns those switches now, and writing a setting writes
/// the cvar behind it - but nothing was re-reading them, so the switch moved
/// and the sound did not change until something else called SetCVar.
void applySoundCVarSideEffects(lua_State* L);

/// One CVar's saved value, read straight from the file.
///
/// For the settings that have to be known before the Lua state exists - the
/// shadow map's size is chosen when the per-frame resources are built, which
/// is long before the interface loads. Returns the fallback when the file has
/// no such key, so a fresh install gets the default rather than nothing.
std::string storedCVarValue(const std::string& key, const std::string& fallback);

/// Tell the CVar store that a setting it drives has been changed elsewhere.
///
/// Six settings are driven by a Blizzard control through kClientCVars, and
/// three of those also have a control in this client's own settings window.
/// The store is applied over the settings file at start-up - it is the record
/// of what the player last set - so a change made in this client's window and
/// saved to settings.cfg was undone at the next start by a CVar nobody had
/// touched since.
///
/// Does nothing for a setting no CVar drives, and writes only when the value
/// actually moves: the settings window applies on every frame of a slider drag,
/// and saving the store means writing a file.
void noteClientSettingChanged(const std::string& settingKey, const std::string& value);

/// Put a value in the CVar store and on disk, without the CVAR_UPDATE the
/// interface's own SetCVar announces.
///
/// For the things the player sets by using the interface rather than by
/// setting a setting - the minimap's zoom is the first - which have to be
/// there again on the next start. The store is written on every change for the
/// reason its own file says: a value that survives only a clean exit is not one
/// a player can rely on, and this client is exited every way there is.
void setStoredCVar(const std::string& key, const std::string& value);
void registerActionLuaAPI(lua_State* L);

/// One line of the CVar listing the client's own console command prints
/// (`FUN_00769100("cvarlist", 0x00767220, 5, "List cvars")`).
///
/// The three values are the three the client keeps on its CVar object, at +0x28,
/// +0x40 and +0x4c: what GetCVar answers now, the value the CVar was registered
/// with, and the value it held when the session started. An empty
/// An empty `defaultValue` or `resetValue` is the client's null: that half of the
/// line is not printed at all. The rows have that test applied already, since
/// the client prints a half only where it differs from the current value and
/// compares case-sensitively doing it (0x0076e760).
struct CVarConsoleRow {
    std::string name;
    std::string value;
    std::string defaultValue;
    std::string resetValue;
};

/// Every CVar there is to list: the names the client registers, the ones the
/// interface's RegisterCVar made, and every name holding a value - each answered
/// the way GetCVar answers it, so the listing cannot disagree with a panel.
std::vector<CVarConsoleRow> cVarConsoleRows(lua_State* L);

/// Whether a CVar of that name exists, spelled any way. Case-insensitive, as the
/// client's lookup (0x0055f4d0) is.
bool cVarConsoleHas(const std::string& name);

/// /cvar_reset and /cvar_default (0x00767840, 0x007678f0), one name at a time.
///
/// Each takes the other's target when it has none of its own: the reset reads the
/// startup value and falls back to the registered default, the default the other
/// way round (0x00766990 and 0x00766a20 read +0x4c/+0x40 in the opposite order),
/// and neither writes when the current answer already matches - which the client
/// compares case-insensitively (0x0076e780, the A-Z fold, and not the
/// case-sensitive compare the listing above uses).
///
/// Neither announces CVAR_UPDATE. The client's reset path assigns the CVar's own
/// string and re-parses it (0x0047cf80); the only site in the binary that raises
/// CVAR_UPDATE - script event 298, whose name is registered at 0x00c25358 - is
/// the three-argument SetCVar binding, and it fires the event with its *third*
/// argument. So a reset leaves an open options panel showing where it was until
/// the interface is reloaded, and this client does the same.
void resetStoredCVar(lua_State* L, const std::string& name);
void restoreCVarDefault(lua_State* L, const std::string& name);

/// Whether this client performs a binding command itself, without the
/// interface.
///
/// A key answered by both is answered twice, and most bindings toggle
/// something - so twice means the panel opens and shuts again on one press and
/// the key reads as dead. Consulted before running a binding.
bool clientActsOnBinding(const std::string& command);

/// The ImGui key the client listens on for a binding command, when it is one
/// of the panels KeybindingManager answers; 0 (ImGuiKey_None) otherwise.
///
/// A controller button bound to one of those has to press that key, since the
/// manager asks ImGui rather than the binding table.
int clientImGuiKeyForBinding(const std::string& command);

} // namespace wowee::addons
