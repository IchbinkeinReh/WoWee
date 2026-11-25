#pragma once

// What the rest of the client asks of the MCP server: whether a thing the
// server may have set off is to be done.
//
// A few questions, null while no server runs - which is always, but for a start
// with --mcp - so that a client without one pays a comparison and none of what
// asks here has to be built with the server.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wowee::mcp::gate {

/// Whether a packet may go out (WorldSocket::send), by its opcode on the wire.
inline bool (*allowsSend)(uint16_t wireOpcode, std::span<const uint8_t> payload) = nullptr;
/// Whether text typed or kept by somebody may be run as Lua (/run, macros).
inline bool (*allowsLua)() = nullptr;
/// Whether the client may be ended from inside it.
inline bool (*allowsQuit)() = nullptr;
/// A setting is changed (settings_ledger.hpp), of which kind of account data
/// or, with -1, none; true: put back as the client ends.
inline bool (*settingChanged)(const std::string& name, const std::string& before, const std::string& after,
                              int accountData) = nullptr;
/// Whether a kind of account data, changed here, may be sent up; nothing has
/// changed where both are the same.
inline bool (*allowsAccountData)(uint32_t type, const std::string& before, const std::string& after) = nullptr;
/// Every event the interface is told (LuaEngine), with its arguments as text.
inline void (*heardEvent)(const std::string& event, const std::vector<std::string>& args) = nullptr;

inline bool sendAllowed(uint16_t wireOpcode, std::span<const uint8_t> payload) {
    return allowsSend == nullptr || allowsSend(wireOpcode, payload);
}
inline bool noteSetting(const std::string& name, const std::string& before, const std::string& after,
                        int accountData) {
    return settingChanged != nullptr && settingChanged(name, before, after, accountData);
}
inline bool accountDataAllowed(uint32_t type, const std::string& before, const std::string& after) {
    return allowsAccountData == nullptr || allowsAccountData(type, before, after);
}
inline bool luaAllowed() { return allowsLua == nullptr || allowsLua(); }
inline bool quitAllowed() { return allowsQuit == nullptr || allowsQuit(); }

}  // namespace wowee::mcp::gate
