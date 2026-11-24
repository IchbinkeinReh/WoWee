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
#include "game/macro_cache.hpp"
#include "game/macro_slots.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/config_paths.hpp"
#include "game/character.hpp"
#include "game/opcode_table.hpp"
#include "game/world_packets.hpp"
#include "core/logger.hpp"
#include "network/world_socket.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
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

std::string accountMacrosPath(const std::string& account) {
    std::string name;
    for (const char ch : account) {
        const auto byte = static_cast<unsigned char>(ch);
        name += std::isalnum(byte) ? static_cast<char>(std::toupper(byte)) : '_';
    }
    return core::getConfigRoot() + "/accounts/" + name + "/macros-cache.txt";
}

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
    std::vector<macro_cache::Macro> macros;
    for (const uint32_t id : macro_list::order(macroList(), perCharacter)) {
        std::string icon = getMacroIcon(id);
        if (const size_t slash = icon.find_last_of("\\/"); slash != std::string::npos) icon = icon.substr(slash + 1);
        macros.push_back({id, getMacroName(id), icon, getMacroText(id)});
    }
    return macro_cache::write(macros);
}

void GameHandler::putMacros(bool perCharacter, const std::string& cacheText) {
    for (auto* kept : {&macros_, &macroNames_, &macroIcons_})
        std::erase_if(*kept, [&](const auto& m) { return macro_list::perCharacter(m.first) == perCharacter; });
    for (const auto& macro : macro_cache::read(cacheText, perCharacter)) {
        if (!macro.body.empty()) macros_[macro.id] = macro.body;
        macroNames_[macro.id] = macro.name;
        if (!macro.icon.empty()) macroIcons_[macro.id] = "Interface\\Icons\\" + macro.icon;
    }
}

void GameHandler::applyAccountDataMacros(bool perCharacter, const std::string& text) {
    putMacros(perCharacter, text);
    if (!perCharacter) {
        accountMacrosOf_ = accountName;
        saveAccountMacros();
    }
    saveCharacterConfig();
    settleMacroSlots(*this);
    fireAddonEvent("UPDATE_MACROS", {});
}

bool GameHandler::loadAccountMacros() {
    if (accountName.empty()) return false;
    if (accountMacrosOf_ == accountName) return accountMacrosKept_;
    // Another account's are not this one's.
    putMacros(false, {});
    accountMacrosOf_ = accountName;
    accountMacrosKept_ = false;
    accountMacrosWritten_.clear();
    std::ifstream in(accountMacrosPath(accountName), std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream text;
    text << in.rdbuf();
    putMacros(false, text.str());
    accountMacrosWritten_ = accountDataMacros(false);
    accountMacrosKept_ = true;
    return true;
}

void GameHandler::saveAccountMacros() {
    if (accountName.empty() || accountMacrosOf_ != accountName) return;
    std::string text = accountDataMacros(false);
    if (text == accountMacrosWritten_) return;
    const std::filesystem::path path = accountMacrosPath(accountName);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARNING("Could not save the account's macros to ", path.string());
        return;
    }
    out << text;
    accountMacrosWritten_ = std::move(text);
    accountMacrosKept_ = true;
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
    // What is compared here is this account's and this character's: the files
    // are read before, as the client reads its own (0x005668f0). An account
    // that has none yet takes the realm's macros.
    const bool accountMacrosHere = loadAccountMacros();
    if (ch && (data.mask & ~kGlobalMask) != 0 && characterConfigOf_ != playerGuid) loadCharacterConfig();
    for (uint32_t t = 0; t < kAccountDataTypes; ++t) {
        if ((data.mask & (1u << t)) == 0) continue;
        if (!kTypes[t].global && !getActiveCharacter()) continue;
        accountDataKnown_[t] = true;
        if (!accountDataSynced(t)) continue;
        const uint32_t local = t == 4 && !accountMacrosHere ? 0 : accountDataLocalTime(t);
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
