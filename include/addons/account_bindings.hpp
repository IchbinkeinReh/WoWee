#pragma once

#include <array>
#include <map>
#include <string>

namespace wowee::addons {

/// command -> its two keys, empty meaning not bound: the key bindings as the
/// Key Bindings panel shows them.
using BindingKeyMap = std::map<std::string, std::array<std::string, 2>>;

/// The key to the command, or to nothing for "NONE" or an empty command, as
/// 0x00562ed0 binds it: the key leaves whatever command held it first, since
/// a key belongs to one command at a time.
inline void placeBindingKey(BindingKeyMap& keys, const std::string& key, const std::string& command) {
    if (key.empty()) return;
    for (auto& [held, slots] : keys)
        for (std::string& slot : slots)
            if (slot == key) slot.clear();
    if (command.empty() || command == "NONE") return;
    auto& slots = keys[command];
    if (slots[0].empty()) slots[0] = key;
    else slots[1] = key;
}

/// bindings-cache.wtf as the realm keeps it (account data 2), read the way
/// 0x005641c0 reads it: the defaults first (0x00562b80 copies the default set
/// over the one being loaded), then each "bind KEY COMMAND" line on top.
///
/// The original writes only the keys that differ from the defaults
/// (0x0055f1b0 skips the entries 0x00562ed0 marked as set from them), so the
/// text alone is never the whole list: a realm copy holding one rebound key
/// leaves every other key on its default.
inline BindingKeyMap bindingsFromCacheText(const BindingKeyMap& defaults, const std::string& text) {
    BindingKeyMap keys = defaults;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, 5, "bind ") != 0) continue;
        const size_t keyAt = start + 5;
        const size_t sp = line.find(' ', keyAt);
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(keyAt, sp - keyAt);
        const std::string command = line.substr(sp + 1);
        if (key.empty()) continue;
        placeBindingKey(keys, key, command);
    }
    return keys;
}

/// The text 0x0055f1b0 writes for bindings-cache.wtf: the binding mode, then a
/// bind line for each key that does not hold its default - "NONE" for a
/// default key taken off its command - each ended by CRLF as the original
/// ends them. Nothing at all when every key holds its default: the mode line
/// goes out only ahead of the first bind line.
inline std::string bindingsCacheText(const BindingKeyMap& defaults, const BindingKeyMap& current) {
    std::map<std::string, std::string> wanted, standard;
    for (const auto& [command, slots] : defaults)
        for (const std::string& key : slots)
            if (!key.empty()) standard[key] = command;
    for (const auto& [command, slots] : current)
        for (const std::string& key : slots)
            if (!key.empty()) wanted[key] = command;
    std::map<std::string, std::string> lines;
    for (const auto& [key, command] : wanted) {
        const auto it = standard.find(key);
        if (it == standard.end() || it->second != command) lines[key] = command;
    }
    for (const auto& [key, command] : standard) {
        (void)command;
        if (!wanted.count(key)) lines[key] = "NONE";
    }
    if (lines.empty()) return {};
    std::string out = "BINDINGMODE 0\r\n";
    for (const auto& [key, command] : lines) out += "bind " + key + " " + command + "\r\n";
    return out;
}

}  // namespace wowee::addons
