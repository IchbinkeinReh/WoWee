// lua_commentator_api.cpp - The arena commentator's functions
// (CommentatorFrame.cpp, 0x00568730-0x0056b0b0), as wow.exe 12340 answers
// them. Most answer only a commentator - PLAYER_FLAGS 0x80000, the
// tournament's 0x400000 for the instance commands - and read the maps and
// instances SMSG_COMMENTATOR_MAP_INFO and _PLAYER_INFO send, which this
// client does not take: it knows no maps, so no instance is ever selected
// and the calls that need one say so as the client does without one.
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"

#include <algorithm>
#include <optional>

namespace wowee::addons {

namespace {

constexpr uint32_t kCommentator = 0x80000;   ///< PLAYER_FLAGS bit 19
constexpr uint32_t kTournament = 0x400000;   ///< PLAYER_FLAGS bit 22

/// The commentator's state the functions keep (0x00ace4b4-0x00ace538).
struct CommentatorState {
    float cameraX = 0.0f, cameraY = 0.0f, cameraZ = 0.0f;  ///< 0x00ace4b4
    float cameraYaw = 0.0f, cameraPitch = 0.0f;
    uint64_t lookAt = 0;     ///< 0x00ace4d0
    uint64_t follow = 0;     ///< 0x00ace4d8
    float fov = 0.0f;        ///< 0x00ace4e4
    float zoom = 0.0f;       ///< 0x00ace4e8
    bool collision = false;  ///< 0x00ace4f0
    float heightOffset = 0.0f;  ///< 0x00ace4f4
    float moveSpeed = 0.0f;
    uint64_t battlemaster = 0;  ///< 0x00ace520
    uint32_t skirmishMode = 0;  ///< 0x00ace538
};

CommentatorState& state() {
    static CommentatorState s;
    return s;
}

/// The maps SMSG_COMMENTATOR_MAP_INFO would have listed (0x00ace4fc).
constexpr uint32_t kNumMaps = 0;

bool isCommentator(game::GameHandler* gh) {
    const auto f = playerFlagsOf(gh);
    return f && (*f & kCommentator) != 0;
}

bool isTournament(game::GameHandler* gh) {
    const auto f = playerFlagsOf(gh);
    return f && (*f & kTournament) != 0;
}

/// 0x006de980: a commentator, either of the tournament or on an arena
/// map (Map.dbc type 4).
bool commentatorCamera(game::GameHandler* gh) {
    const auto f = playerFlagsOf(gh);
    return f && (*f & kCommentator) != 0 && ((*f & kTournament) != 0 || gh->currentMapInstanceType() == 4);
}

void send(game::GameHandler* gh, network::Packet& packet) {
    if (gh) gh->sendCommentatorRequest(packet);
}

/// CMSG_COMMENTATOR_ENABLE: 0 off, 1 on, 2 the other way (0x005690a0,
/// 0x00569180).
void sendEnable(game::GameHandler* gh, uint32_t mode) {
    if (!gh || game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_ENABLE) == 0xFFFF) return;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_ENABLE));
    packet.writeUInt32(mode);
    send(gh, packet);
}

/// CMSG_COMMENTATOR_SKIRMISH_QUEUE_COMMAND (0x0056ac90 ff.).
void sendSkirmish(game::GameHandler* gh, uint32_t command, uint32_t value, uint64_t a, uint64_t b, uint32_t last) {
    if (!gh || game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_SKIRMISH_QUEUE_COMMAND) == 0xFFFF) return;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_SKIRMISH_QUEUE_COMMAND));
    packet.writeUInt32(command);
    packet.writeUInt32(value);
    packet.writeUInt64(a);
    packet.writeUInt64(b);
    packet.writeUInt32(last);
    send(gh, packet);
}

int numberArgs(lua_State* L, int n) {
    for (int i = 1; i <= n; ++i)
        if (!lua_isnumber(L, i)) return 0;
    return 1;
}

int index(lua_State* L, int i) { return static_cast<int>(std::lround(lua_tonumber(L, i))); }

int getNumMaps(lua_State* L) {
    lua_pushnumber(L, kNumMaps);
    return 1;
}

/// CommentatorZoomIn/Out (0x00568770, 0x00568810): the camera's zoom taken
/// a tenth of the field of view (at least 20 degrees) in, or eleven
/// hundredths of it out, no further than 1.1 or 0.9 times the field of
/// view, held to 1-120 degrees.
template <bool In>
int zoom(lua_State*) {
    auto& s = state();
    constexpr float kMin = 0.017453292f, kMax = 2.0943952f;
    const float base = std::max(s.fov, 0.34906584f);
    const float to = In ? std::min(s.fov * 1.1f, s.zoom - base * 0.1f) : std::max(s.fov * 0.9f, s.zoom + base * 1.1f * 0.1f);
    s.zoom = std::clamp(to, kMin, kMax);
    return 0;
}

int setMode(lua_State* L) {
    if (lua_isnumber(L, 1)) sendEnable(getGameHandler(L), lua_tonumber(L, 1) != 0 ? 1 : 0);
    return 0;
}

int toggleMode(lua_State* L) {
    sendEnable(getGameHandler(L), 2);
    return 0;
}

/// CommentatorGetMode (0x00569230): 1 a commentator, 2 of the tournament.
int getMode(lua_State* L) {
    const auto f = playerFlagsOf(getGameHandler(L));
    int mode = 0;
    if (f && (*f & kCommentator) != 0) mode = 1;
    if (f && (*f & kTournament) != 0) mode = 2;
    lua_pushnumber(L, mode);
    return 1;
}

/// CommentatorUpdateMapInfo([name]) (0x00569340): CMSG_COMMENTATOR_GET_MAP_INFO.
int updateMapInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isCommentator(gh) || game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_GET_MAP_INFO) == 0xFFFF) return 0;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_GET_MAP_INFO));
    packet.writeString(lua_isstring(L, 1) ? lua_tostring(L, 1) : "");
    send(gh, packet);
    return 0;
}

/// The calls that read a map: none is known.
int getMapInfo(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CommentatorGetMapInfo(mapIndex)");
    return luaL_error(L, "no map at that index");
}

int getInstanceInfo(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    if (!numberArgs(L, 2)) return luaL_error(L, "Usage: CommentatorGetMapInfo(mapIndex,instanceIndex)");
    return luaL_error(L, "no map at that index");
}

int setMapAndInstanceIndex(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    if (!numberArgs(L, 2)) return luaL_error(L, "Usage: CommentatorSetMapAndInstanceIndex(mapIndex,instanceIndex)");
    return luaL_error(L, "no map at that index");
}

/// The calls that read the selected instance (0x005688c0): there is none.
int enterInstance(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    return luaL_error(L, "Unable to enter instance.");
}

int exitInstance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isCommentator(gh) || game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_EXIT_INSTANCE) == 0xFFFF) return 0;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_EXIT_INSTANCE));
    send(gh, packet);
    return 0;
}

/// CommentatorGetNumPlayers(faction) (0x00569820): the instance's players
/// of one side - none without an instance.
int getNumPlayers(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CommentatorGetNumPlayers(factionIndex)");
    if (static_cast<unsigned>(index(L, 1) - 1) >= 2) return luaL_error(L, "Error: faction index too large");
    lua_pushnumber(L, 0);
    return 1;
}

int getPlayerInfo(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    if (!numberArgs(L, 2)) return luaL_error(L, "Usage: CommentatorGetPlayerInfo(factionIndex,playerIndex)");
    if (static_cast<unsigned>(index(L, 1) - 1) > 1) return luaL_error(L, "Error: faction index too large");
    return luaL_error(L, "Unable to find instance.");
}

int setPlayerIndex(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    if (!numberArgs(L, 2)) return luaL_error(L, "Usage: CommentatorSetPlayerIndex(factionIndex,playerIndex)");
    if (static_cast<unsigned>(index(L, 1) - 1) > 1) return luaL_error(L, "Error: faction index too large");
    return luaL_error(L, "Unable to find instance.");
}

/// CommentatorFollowPlayer / LookatPlayer (0x00569b50, 0x00569cd0): a
/// player index of 0 lets go of both; any other needs the instance.
template <bool Follow>
int watchPlayer(lua_State* L) {
    if (!commentatorCamera(getGameHandler(L))) return 0;
    if (!numberArgs(L, 2))
        return luaL_error(L, Follow ? "Usage: CommentatorFollowPlayer(factionIndex,playerIndex)"
                                    : "Usage: CommentatorLookatPlayer(factionIndex,playerIndex)");
    if (static_cast<unsigned>(index(L, 1) - 1) >= 2) return luaL_error(L, "Error: faction index too large");
    if (index(L, 2) == 0) {
        state().follow = 0;
        state().lookAt = 0;
        return 0;
    }
    return luaL_error(L, "Unable to find instance.");
}

/// CommentatorSetCamera(x, y, z, yaw, pitch, fov) (0x0056a0f0): the
/// commentator's camera put there, its field of view held to 1-120
/// degrees.
int setCamera(lua_State* L) {
    if (!commentatorCamera(getGameHandler(L))) return 0;
    if (!numberArgs(L, 6)) return luaL_error(L, "Usage: CommentatorSetCamera(xPos,yPos,zPos,yaw,pitch,fov)");
    auto& s = state();
    s.cameraX = static_cast<float>(lua_tonumber(L, 1));
    s.cameraY = static_cast<float>(lua_tonumber(L, 2));
    s.cameraZ = static_cast<float>(lua_tonumber(L, 3));
    s.cameraYaw = static_cast<float>(lua_tonumber(L, 4)) * 0.017453292f;
    s.cameraPitch = static_cast<float>(lua_tonumber(L, 5)) * 0.017453292f;
    const float fov = static_cast<float>(lua_tonumber(L, 6)) * 0.017453292f;
    s.fov = fov > 0.017453292f ? std::min(fov, 2.0943952f) : 0.017453292f;
    return 0;
}

/// CommentatorGetCamera (0x0056a2a0): x, y, z, yaw, pitch, field of view.
int getCamera(lua_State* L) {
    if (!commentatorCamera(getGameHandler(L))) return 0;
    const auto& s = state();
    lua_pushnumber(L, s.cameraX);
    lua_pushnumber(L, s.cameraY);
    lua_pushnumber(L, s.cameraZ);
    lua_pushnumber(L, s.cameraYaw * 57.29578f);
    lua_pushnumber(L, s.cameraPitch * 57.29578f);
    lua_pushnumber(L, s.fov * 57.29578f);
    return 6;
}

/// CommentatorGetCurrentMapID (0x0056a380): the selected instance's map.
int getCurrentMapID(lua_State*) { return 0; }

/// CommentatorStartInstance(mapID, teamSize, minLevel, maxLevel)
/// (0x0056a410): of the tournament, with a battlemaster set, a team of 2,
/// 3 or 5 - CMSG_COMMENTATOR_INSTANCE_COMMAND 0.
int startInstance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isTournament(gh)) return 0;
    if (state().battlemaster == 0) return luaL_error(L, "Battlemaster must be set first.");
    if (!numberArgs(L, 4)) return luaL_error(L, "Usage: CommentatorStartInstance(mapID,teamSize,minLevel,maxLevel)");
    const int teamSize = index(L, 2);
    if (teamSize != 2 && teamSize != 3 && teamSize != 5) return luaL_error(L, "Unknown team size. Use 2, 3, or 5.");
    if (game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_INSTANCE_COMMAND) == 0xFFFF) return 0;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_COMMENTATOR_INSTANCE_COMMAND));
    packet.writeUInt64(0);
    packet.writeUInt64(0);
    packet.writeUInt64(state().battlemaster);
    packet.writeUInt32(0);
    packet.writeUInt32(static_cast<uint32_t>(index(L, 1)));
    packet.writeUInt32(static_cast<uint32_t>(teamSize));
    packet.writeUInt32(static_cast<uint32_t>(index(L, 3)));
    packet.writeUInt32(static_cast<uint32_t>(index(L, 4)));
    packet.writeUInt32(0);
    send(gh, packet);
    return 0;
}

/// CommentatorAddPlayer(team) (0x0056a640): the target, a player, put in
/// the selected instance - none is.
int addPlayer(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isTournament(gh)) return 0;
    if (gh->getTargetGuid() == 0) return luaL_error(L, "A player must be targeted.");
    const auto target = gh->getEntityManager().getEntity(gh->getTargetGuid());
    if (!target || target->getType() != game::ObjectType::PLAYER) return luaL_error(L, "Must target a player.");
    return luaL_error(L, "Must select an instance first.");
}

int removePlayer(lua_State* L) {
    if (!isTournament(getGameHandler(L))) return 0;
    return luaL_error(L, "Must select an instance first.");
}

/// CommentatorSetBattlemaster (0x0056aa10): the player's target, where it
/// is a unit flagged a battlemaster (UNIT_NPC_FLAGS 0x100000).
int setBattlemaster(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isCommentator(gh)) return 0;
    const uint64_t target = gh->getTargetGuid();
    if (target == 0) return luaL_error(L, "Battlemaster must be targeted.");
    const auto unit = gh->getEntityManager().getEntity(target);
    const uint16_t npcFlags = game::fieldIndex(game::UF::UNIT_NPC_FLAGS);
    if (!unit || !unit->isUnit() || npcFlags == 0xFFFF || (unit->getField(npcFlags) & 0x100000u) == 0)
        return luaL_error(L, "Targeted unit is not a Battlemaster.");
    state().battlemaster = target;
    return 0;
}

int setMoveSpeed(lua_State* L) {
    if (!commentatorCamera(getGameHandler(L))) return 0;
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CommentatorSetMoveSpeed(speed)");
    state().moveSpeed = static_cast<float>(lua_tonumber(L, 1));
    return 0;
}

int setCameraCollision(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isCommentator(gh) || !isTournament(gh)) return 0;
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CommentatorSetCameraCollision(bool enable)");
    state().collision = lua_tonumber(L, 1) != 0.0;
    return 0;
}

int setTargetHeightOffset(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isCommentator(gh) || !isTournament(gh)) return 0;
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CommentatorSetTargetHeightOffset(float offset)");
    state().heightOffset = static_cast<float>(lua_tonumber(L, 1));
    return 0;
}

int setSkirmishMatchmakingMode(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isCommentator(gh) || !isTournament(gh) || !lua_isnumber(L, 1)) return 0;
    sendSkirmish(gh, 0, static_cast<uint32_t>(index(L, 1)) & 0xFFu, 0, 0, 0);
    return 0;
}

int requestSkirmishQueueData(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (isCommentator(gh)) sendSkirmish(gh, 1, 1, 0, 0, 0);
    return 0;
}

int requestSkirmishMode(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (isCommentator(gh)) sendSkirmish(gh, 3, 1, 0, 0, 0);
    return 0;
}

int getSkirmishMode(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    lua_pushnumber(L, state().skirmishMode);
    return 1;
}

/// The skirmish queue SMSG_COMMENTATOR_SKIRMISH_QUEUE_RESULT1 would list
/// (0x00ace52c): empty.
int getSkirmishQueueCount(lua_State* L) {
    if (!isCommentator(getGameHandler(L))) return 0;
    lua_pushnumber(L, 0);
    return 1;
}

int getSkirmishQueuePlayerInfo(lua_State*) { return 0; }

/// CommentatorStartSkirmishMatch(name1, name2[, n]) (0x0056b0b0): command 2
/// with the two players by name (0x0074d120).
int startSkirmishMatch(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!isCommentator(gh) || !lua_isstring(L, 1) || !lua_isstring(L, 2)) return 0;
    const uint64_t a = resolveUnitOrName(gh, lua_tostring(L, 1));
    const uint64_t b = resolveUnitOrName(gh, lua_tostring(L, 2));
    const uint32_t n = lua_isnumber(L, 3) ? static_cast<uint32_t>(lua_tonumber(L, 3)) : 0xFFFFFFFFu;
    sendSkirmish(gh, 2, 1, a, b, n);
    return 0;
}

}  // namespace

void registerCommentatorLuaAPI(lua_State* L) {
    static const luaL_Reg fns[] = {
        {"CommentatorGetNumMaps",                 getNumMaps},
        {"CommentatorZoomIn",                     zoom<true>},
        {"CommentatorZoomOut",                    zoom<false>},
        {"CommentatorSetMode",                    setMode},
        {"CommentatorToggleMode",                 toggleMode},
        {"CommentatorGetMode",                    getMode},
        {"CommentatorUpdatePlayerInfo",           [](lua_State*) -> int { return 0; }},
        {"CommentatorUpdateMapInfo",              updateMapInfo},
        {"CommentatorGetMapInfo",                 getMapInfo},
        {"CommentatorGetInstanceInfo",            getInstanceInfo},
        {"CommentatorEnterInstance",              enterInstance},
        {"CommentatorExitInstance",               exitInstance},
        {"CommentatorGetNumPlayers",              getNumPlayers},
        {"CommentatorGetPlayerInfo",              getPlayerInfo},
        {"CommentatorFollowPlayer",               watchPlayer<true>},
        {"CommentatorLookatPlayer",               watchPlayer<false>},
        {"CommentatorSetMapAndInstanceIndex",     setMapAndInstanceIndex},
        {"CommentatorSetPlayerIndex",             setPlayerIndex},
        {"CommentatorSetCamera",                  setCamera},
        {"CommentatorGetCamera",                  getCamera},
        {"CommentatorGetCurrentMapID",            getCurrentMapID},
        {"CommentatorStartInstance",              startInstance},
        {"CommentatorAddPlayer",                  addPlayer},
        {"CommentatorRemovePlayer",               removePlayer},
        {"CommentatorSetBattlemaster",            setBattlemaster},
        {"CommentatorSetMoveSpeed",               setMoveSpeed},
        {"CommentatorSetCameraCollision",         setCameraCollision},
        {"CommentatorSetTargetHeightOffset",      setTargetHeightOffset},
        {"CommentatorSetSkirmishMatchmakingMode", setSkirmishMatchmakingMode},
        {"CommentatorRequestSkirmishQueueData",   requestSkirmishQueueData},
        {"CommentatorRequestSkirmishMode",        requestSkirmishMode},
        {"CommentatorGetSkirmishMode",            getSkirmishMode},
        {"CommentatorGetSkirmishQueueCount",      getSkirmishQueueCount},
        {"CommentatorGetSkirmishQueuePlayerInfo", getSkirmishQueuePlayerInfo},
        {"CommentatorStartSkirmishMatch",         startSkirmishMatch},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* fn = fns; fn->name; ++fn) {
        lua_pushcfunction(L, fn->func);
        lua_setglobal(L, fn->name);
    }
}

}  // namespace wowee::addons
