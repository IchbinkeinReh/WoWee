// lua_absent_api.cpp - The client's answers from subsystems this client does
// not have: voice chat, Battle.net, the knowledge base, script CPU profiling
// and the scripts' file access. Each is what wow.exe 12340 answers with the
// subsystem off or never loaded, read from its function.
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"

namespace wowee::addons {

namespace {

int nothing(lua_State*) { return 0; }

// ---- Voice chat ----
//
// Voice is off here: VoiceIsDisabledByClient answers true, and the sound
// system has no capture driver.

/// IsVoiceChatAllowed() (0x004fcc60): 1 when the server's feature status
/// allows voice (0xbcf004) and the client has not disabled it (0xd37d18),
/// else nil - and it is disabled by this client.
int isVoiceChatAllowed(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

/// VoiceGetCurrentCaptureDevice() / VoiceGetCurrentOutputDevice()
/// (0x007dafe0, 0x007daf90): -1 while voice is not running (0xd37d24).
int noVoiceDevice(lua_State* L) {
    lua_pushnumber(L, -1);
    return 1;
}

/// Sound_GameSystem_GetNumInputDrivers() (0x00985bb0): the capture
/// drivers the sound system found (0x004c5a60) - none.
int numInputDrivers(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

/// Sound_GameSystem_GetInputDriverNameByIndex(index) (0x00985be0): the
/// driver's name into an empty buffer - "" for one that is not there.
int inputDriverName(lua_State* L) {
    if (!lua_isnumber(L, 1))
        return luaL_error(L, "Usage: Sound_GameSystem_GetInputDriverNameByIndex(InputDriverIndex)");
    lua_pushstring(L, "");
    return 1;
}

// ---- Battle.net ----
//
// Each of these does its work only with the Battle.net connection up (its
// state 1, 0x006b0990), which it never is here, and then answers nothing -
// not even a usage error.

/// IsBNLogin() (0x00530f20): whether the connection is in state 1.
int isBNLogin(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

// ---- Knowledge base ----
//
// Never loaded: setup (0xacd53c), query (0xacd5b4) and article (0xacd604)
// stay short of their loaded state, 2.

/// KBSystem_GetServerStatus() (0x00551fd0): the status text the server
/// sent (0xbea810), none.
int kbServerStatus(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

/// KBArticle_IsLoaded() / KBQuery_IsLoaded() (0x00551da0, 0x00551b90).
int kbNotLoaded(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

/// The setup and query readers raise while nothing is loaded
/// (0x00551790, 0x005517d0, 0x00551a30, 0x00551c00).
int kbLanguageCount(lua_State* L) {
    return luaL_error(L, "KBSetup_GetLanguageCount() failed because setup is not loaded");
}
int kbLanguageData(lua_State* L) {
    return luaL_error(L, "KBSetup_GetLanguageData() failed because setup is not loaded");
}
int kbSetupArticleHeader(lua_State* L) {
    return luaL_error(L, "KBSetup_GetArticleHeaderData() failed because setup is not loaded");
}
int kbQueryArticleHeader(lua_State* L) {
    return luaL_error(L, "KBQuery_GetArticleHeaderData() failed because query is not loaded");
}

// ---- Script CPU profiling ----
//
// The counters only run with the scriptProfile cvar on, which nothing here
// turns on, so every reading is zero - after the same argument checks.

/// GetAddOnCPUUsage(index or "name") (0x00511660): an index outside the
/// addons raises; a name is taken as it is.
int addOnCPUUsage(lua_State* L) {
    if (lua_isnumber(L, 1)) {
        lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_count");
        const int count = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        const int index = static_cast<int>(lua_tonumber(L, 1));
        if (index < 1 || index > count)
            return luaL_error(L, "AddOn index must be in the range of 1 to %d", count);
    } else if (!lua_isstring(L, 1)) {
        return luaL_error(L, "Usage: GetAddOnCPUUsage(index or \"name\")");
    }
    lua_pushnumber(L, 0);
    return 1;
}

/// GetEventCPUUsage(["event"]) (0x005117c0): time and count.
int eventCPUUsage(lua_State* L) {
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 2;
}

/// GetFrameCPUUsage(frame[, includeChildren]) (0x00517890): a frame's
/// table, else its usage error; time and count.
int frameCPUUsage(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "Usage: GetFrameCPUUsage(frame[, includeChildren])");
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 2;
}

/// GetFunctionCPUUsage(function[, includeSubroutines]) (0x00511720).
int functionCPUUsage(lua_State* L) {
    if (!lua_isfunction(L, 1))
        return luaL_error(L, "Usage: GetFunctionCPUUsage(function[, includeSubroutines])");
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 2;
}

/// GetScriptCPUUsage() (0x00511610).
int scriptCPUUsage(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

// ---- Minigames ----
//
// No minigame is ever running here (0xc20fec stays null).

/// GetMinigameType() (0x005c5500): the running game's type, nil without.
int minigameType(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

/// MakeMinigameMove(moveType, param) (0x005c5550): two numbers, else its
/// usage error; a move in no game does nothing.
int minigameMove(lua_State* L) {
    if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2))
        return luaL_error(L, "Usage: MakeMinigameMove(moveType, param)");
    return 0;
}

// ---- The scripts' file access ----

/// AppendToFile, DeleteFile, ReadFile and GMRequestPlayerInfo share one
/// function (0x00510bb0): refused.
int accessDenied(lua_State* L) {
    return luaL_error(L, "Access Denied");
}

}  // namespace

void registerAbsentLuaAPI(lua_State* L) {
    static const luaL_Reg fns[] = {
        {"IsVoiceChatAllowed",                       isVoiceChatAllowed},
        {"VoiceGetCurrentCaptureDevice",             noVoiceDevice},
        {"VoiceGetCurrentOutputDevice",              noVoiceDevice},
        {"Sound_GameSystem_GetNumInputDrivers",      numInputDrivers},
        {"Sound_GameSystem_GetInputDriverNameByIndex", inputDriverName},
        // Capture started and stopped on a voice system that is not there
        // (0x00985ef0, 0x00985d50, 0x00985e90), and push-to-talk, which
        // only logs (0x00511d20).
        {"VoiceChat_StartCapture",                   nothing},
        {"VoiceChat_StopCapture",                    nothing},
        {"VoiceChat_ActivatePrimaryCaptureCallback", nothing},
        {"VoicePushToTalkStart",                     nothing},
        {"VoicePushToTalkStop",                      nothing},
        {"IsBNLogin",                                isBNLogin},
        // 0x005364e0, 0x00535700, 0x00535c60.
        {"BNIsBlocked",                              nothing},
        {"BNReportFriendInvite",                     nothing},
        {"BNSetFocus",                               nothing},
        {"KBSystem_GetServerStatus",                 kbServerStatus},
        {"KBArticle_IsLoaded",                       kbNotLoaded},
        {"KBQuery_IsLoaded",                         kbNotLoaded},
        {"KBSetup_GetLanguageCount",                 kbLanguageCount},
        {"KBSetup_GetLanguageData",                  kbLanguageData},
        {"KBSetup_GetArticleHeaderData",             kbSetupArticleHeader},
        {"KBQuery_GetArticleHeaderData",             kbQueryArticleHeader},
        {"GetAddOnCPUUsage",                         addOnCPUUsage},
        {"GetEventCPUUsage",                         eventCPUUsage},
        {"GetFrameCPUUsage",                         frameCPUUsage},
        {"GetFunctionCPUUsage",                      functionCPUUsage},
        {"GetScriptCPUUsage",                        scriptCPUUsage},
        // Resetting and refreshing counters that never ran (0x00511830,
        // 0x00511640).
        {"ResetCPUUsage",                            nothing},
        {"UpdateAddOnCPUUsage",                      nothing},
        // GetMinigameState() (0x005c55f0) asks the game, and there is none.
        {"GetMinigameState",                         nothing},
        {"GetMinigameType",                          minigameType},
        {"MakeMinigameMove",                         minigameMove},
        {"AppendToFile",                             accessDenied},
        {"DeleteFile",                               accessDenied},
        {"ReadFile",                                 accessDenied},
        {"GMRequestPlayerInfo",                      accessDenied},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* fn = fns; fn->name; ++fn) {
        lua_pushcfunction(L, fn->func);
        lua_setglobal(L, fn->name);
    }
}

}  // namespace wowee::addons
