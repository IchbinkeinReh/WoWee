// Account data on the server (AccountData.cpp in the client): the settings the
// client keeps in its cache files - config, key bindings and macros, each for
// the account and for the character - exchanged with the realm so they follow
// the account to another computer.
//
//   CMSG_READY_FOR_ACCOUNT_DATA_TIMES (0x004de390) - asks for the times
//   SMSG_ACCOUNT_DATA_TIMES (0x006b94c0)            - the server's time per type
//   CMSG_REQUEST_ACCOUNT_DATA (0x006b92c0)          - a newer copy wanted
//   SMSG_UPDATE_ACCOUNT_DATA (0x006b9730)           - one arrives, zlib
//   CMSG_UPDATE_ACCOUNT_DATA (0x006b9100)           - the local one sent up

#include "game/game_handler.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/config_paths.hpp"
#include "game/character.hpp"
#include "game/opcode_table.hpp"
#include "game/world_packets.hpp"
#include "core/logger.hpp"
#include "network/world_socket.hpp"

#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>

namespace wowee {
namespace game {

namespace {

/// The client's table (0x00ad94e0, 0x40 a row): which types are the
/// account's, their synchronize CVar, and the one that never leaves the
/// computer (layout-local.txt).
struct AccountDataType {
    bool global;
    const char* syncCVar;   // null: no switch of its own
    bool localOnly;
};
constexpr AccountDataType kTypes[GameHandler::kAccountDataTypes] = {
    {true,  "synchronizeConfig",   false},  // 0 config-cache.wtf, account
    {false, "synchronizeConfig",   false},  // 1 config-cache.wtf, character
    {true,  "synchronizeBindings", false},  // 2 bindings-cache.wtf, account
    {false, "synchronizeBindings", false},  // 3 bindings-cache.wtf, character
    {true,  "synchronizeMacros",   false},  // 4 macros-cache.txt, account
    {false, "synchronizeMacros",   false},  // 5 macros-cache.txt, character
    {false, nullptr,               true},   // 6 layout-local.txt
    {false, nullptr,               false},  // 7 chat-cache.txt
};

constexpr uint32_t kGlobalMask = 0x15;   // 0, 2 and 4

bool cvarOn(const char* name) {
    return addons::storedCVarValue(name, "1") != "0";
}

std::string timesPath() { return core::getConfigRoot() + "/account_data_times.txt"; }

std::vector<uint8_t> deflateText(const std::string& text) {
    uLongf size = compressBound(static_cast<uLong>(text.size()));
    std::vector<uint8_t> out(size);
    if (compress2(out.data(), &size, reinterpret_cast<const Bytef*>(text.data()),
                  static_cast<uLong>(text.size()), Z_DEFAULT_COMPRESSION) != Z_OK) {
        return {};
    }
    out.resize(size);
    return out;
}

std::optional<std::string> inflateText(const std::vector<uint8_t>& data, uint32_t size) {
    std::string out(size, '\0');
    uLongf got = size;
    if (uncompress(reinterpret_cast<Bytef*>(out.data()), &got, data.data(), static_cast<uLong>(data.size())) != Z_OK)
        return std::nullopt;
    out.resize(got);
    return out;
}

}  // namespace

std::string GameHandler::accountDataKey(uint32_t type) const {
    std::string key = accountName;
    if (!kTypes[type].global) {
        const Character* ch = getActiveCharacter();
        key += ":" + (ch ? ch->name : std::string("?"));
    }
    return key + ":" + std::to_string(type);
}

uint32_t GameHandler::accountDataLocalTime(uint32_t type) const {
    std::ifstream in(timesPath());
    std::string line;
    const std::string key = accountDataKey(type) + "=";
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) == 0) return static_cast<uint32_t>(std::strtoul(line.c_str() + key.size(), nullptr, 10));
    }
    return 0;
}

void GameHandler::setAccountDataLocalTime(uint32_t type, uint32_t time) {
    std::vector<std::string> lines;
    {
        std::ifstream in(timesPath());
        std::string line;
        const std::string key = accountDataKey(type) + "=";
        while (std::getline(in, line))
            if (line.rfind(key, 0) != 0 && !line.empty()) lines.push_back(line);
    }
    lines.push_back(accountDataKey(type) + "=" + std::to_string(time));
    std::error_code ec;
    std::filesystem::create_directories(core::getConfigRoot(), ec);
    std::ofstream out(timesPath(), std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
}

bool GameHandler::accountDataSynced(uint32_t type) const {
    if (type >= kAccountDataTypes || kTypes[type].localOnly) return false;
    if (!cvarOn("synchronizeSettings")) return false;
    return !kTypes[type].syncCVar || cvarOn(kTypes[type].syncCVar);
}

std::string GameHandler::accountDataText(uint32_t type) const {
    switch (type) {
        case 0: case 1:
            return accountDataBridge_.config ? accountDataBridge_.config(type == 1) : std::string();
        case 2: case 3:
            return accountDataBridge_.bindings ? accountDataBridge_.bindings(type == 3) : std::string();
        case 4: case 5: return accountDataMacros(type == 5);
        case 7: return accountDataBridge_.chat ? accountDataBridge_.chat() : std::string();
        default: return {};
    }
}

void GameHandler::applyAccountDataText(uint32_t type, const std::string& text) {
    switch (type) {
        case 0: case 1:
            if (accountDataBridge_.applyConfig) accountDataBridge_.applyConfig(type == 1, text);
            break;
        case 2: case 3:
            if (accountDataBridge_.applyBindings) accountDataBridge_.applyBindings(type == 3, text);
            break;
        case 4: case 5: applyAccountDataMacros(type == 5, text); break;
        case 7:
            if (accountDataBridge_.applyChat) accountDataBridge_.applyChat(text);
            break;
        default: break;
    }
}

std::string GameHandler::accountDataMacros(bool perCharacter) const {
    // macros-cache.txt: each macro as MACRO id "name" icon, its body, END.
    std::ostringstream out;
    const uint32_t first = perCharacter ? 37u : 1u, last = perCharacter ? 54u : 36u;
    for (const uint32_t id : getMacroIds()) {
        if (id < first || id > last) continue;
        std::string icon = getMacroIcon(id);
        if (const size_t slash = icon.find_last_of("\\/"); slash != std::string::npos) icon = icon.substr(slash + 1);
        out << "MACRO " << id << " \"" << getMacroName(id) << "\" " << (icon.empty() ? "INV_Misc_QuestionMark" : icon)
            << "\n" << getMacroText(id) << "\nEND\n";
    }
    return out.str();
}

void GameHandler::applyAccountDataMacros(bool perCharacter, const std::string& text) {
    const uint32_t first = perCharacter ? 37u : 1u, last = perCharacter ? 54u : 36u;
    for (const uint32_t id : getMacroIds())
        if (id >= first && id <= last) {
            setMacroText(id, "");
            setMacroMeta(id, "", "");
        }
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("MACRO ", 0) != 0) continue;
        std::istringstream head(line.substr(6));
        uint32_t id = 0;
        head >> id;
        std::string rest;
        std::getline(head, rest);
        const size_t q1 = rest.find('"'), q2 = rest.find('"', q1 == std::string::npos ? 0 : q1 + 1);
        if (id < first || id > last || q1 == std::string::npos || q2 == std::string::npos) continue;
        const std::string name = rest.substr(q1 + 1, q2 - q1 - 1);
        std::string icon = rest.substr(q2 + 1);
        icon.erase(0, icon.find_first_not_of(' '));
        std::string body, l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l == "END") break;
            if (!body.empty()) body += "\n";
            body += l;
        }
        setMacroText(id, body.empty() ? std::string(" ") : body);
        setMacroMeta(id, name, icon.empty() ? std::string() : "Interface\\Icons\\" + icon);
    }
    fireAddonEvent("UPDATE_MACROS", {});
}

void GameHandler::sendReadyForAccountDataTimes() {
    if (!socket || wireOpcode(Opcode::CMSG_READY_FOR_ACCOUNT_DATA_TIMES) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_READY_FOR_ACCOUNT_DATA_TIMES));
    socket->send(packet);
}

void GameHandler::requestAccountData(uint32_t type) {
    if (!socket || wireOpcode(Opcode::CMSG_REQUEST_ACCOUNT_DATA) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_REQUEST_ACCOUNT_DATA));
    packet.writeUInt32(type);
    socket->send(packet);
    accountDataAwaited_[type] = true;
}

void GameHandler::uploadAccountData(uint32_t type) {
    // 0x006b9100: the type, the time (the server's clock as last told, one
    // on for every upload), the size and the zlib'd text.
    if (!socket || wireOpcode(Opcode::CMSG_UPDATE_ACCOUNT_DATA) == 0xFFFF) return;
    const std::string text = accountDataText(type);
    const uint32_t time = accountDataClock_++;
    network::Packet packet(wireOpcode(Opcode::CMSG_UPDATE_ACCOUNT_DATA));
    packet.writeUInt32(type);
    packet.writeUInt32(time);
    packet.writeUInt32(static_cast<uint32_t>(text.size()));
    if (!text.empty()) {
        const std::vector<uint8_t> z = deflateText(text);
        packet.writeBytes(z.data(), z.size());
    }
    socket->send(packet);
    setAccountDataLocalTime(type, time);
    accountDataLastText_[type] = text;
    LOG_INFO("Account data ", type, " sent up (", text.size(), " bytes)");
}

void GameHandler::handleAccountDataTimes(network::Packet& packet) {
    AccountDataTimesData data;
    if (!AccountDataTimesParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_ACCOUNT_DATA_TIMES");
        return;
    }
    // 0x006b94c0: the server's clock, then each type the mask names - a
    // newer copy there is asked for, an older one replaced by this one.
    accountDataClock_ = data.serverTime;
    const Character* ch = getActiveCharacter();
    if (ch && (data.mask & ~kGlobalMask) != 0) accountDataCharacter_ = ch->name;
    for (uint32_t t = 0; t < kAccountDataTypes; ++t) {
        if ((data.mask & (1u << t)) == 0) continue;
        if (!kTypes[t].global && !getActiveCharacter()) continue;
        accountDataKnown_[t] = true;
        if (!accountDataSynced(t)) continue;
        const uint32_t local = accountDataLocalTime(t);
        const uint32_t server = data.accountDataTimes[t];
        if (local < server) requestAccountData(t);
        else if (local > server) uploadAccountData(t);
        else accountDataLastText_[t] = accountDataText(t);
    }
}

void GameHandler::handleUpdateAccountData(network::Packet& packet) {
    // 0x006b9730: the guid, the type, the time, the size and the zlib'd text.
    if (!packet.hasRemaining(8 + 4 + 4 + 4)) return;
    packet.readUInt64();
    const uint32_t type = packet.readUInt32();
    const uint32_t time = packet.readUInt32();
    const uint32_t size = packet.readUInt32();
    if (type >= kAccountDataTypes || !accountDataAwaited_[type]) {
        packet.skipAll();
        return;
    }
    accountDataAwaited_[type] = false;
    std::string text;
    if (size != 0) {
        std::vector<uint8_t> z(packet.getRemainingSize());
        for (auto& b : z) b = packet.readUInt8();
        const auto inflated = inflateText(z, size);
        if (!inflated) {
            LOG_WARNING("Account data ", type, " did not inflate");
            return;
        }
        text = *inflated;
    }
    applyAccountDataText(type, text);
    setAccountDataLocalTime(type, time);
    accountDataLastText_[type] = accountDataText(type);
    LOG_INFO("Account data ", type, " taken from the realm (", text.size(), " bytes)");
}

void GameHandler::tickAccountData(float dt) {
    // A change made here goes up: what the settings say now against what
    // was last sent or taken, looked at once a second.
    accountDataCheckTimer_ -= dt;
    if (accountDataCheckTimer_ > 0.0f) return;
    accountDataCheckTimer_ = 1.0f;
    if (state != WorldState::IN_WORLD) return;
    // Another character's copies are not this one's until the realm says.
    const Character* ch = getActiveCharacter();
    if (!ch || ch->name != accountDataCharacter_) {
        for (uint32_t t = 0; t < kAccountDataTypes; ++t)
            if (!kTypes[t].global) accountDataKnown_[t] = false;
    }
    for (uint32_t t = 0; t < kAccountDataTypes; ++t) {
        if (!accountDataKnown_[t] || accountDataAwaited_[t] || !accountDataSynced(t)) continue;
        const std::string text = accountDataText(t);
        if (text != accountDataLastText_[t]) uploadAccountData(t);
    }
}

}  // namespace game
}  // namespace wowee
