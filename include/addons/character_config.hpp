#pragma once

#include "addons/client_cvar_registry.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::addons {

/// config-cache.wtf's SET lines: the values by lower-case name.
[[nodiscard]] inline std::unordered_map<std::string, std::string> configCacheValues(const std::string& text) {
    std::unordered_map<std::string, std::string> values;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("SET ", 0) != 0) continue;
        const size_t space = line.find(' ', 4);
        if (space == std::string::npos) continue;
        std::string key = line.substr(4, space - 4);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = line.substr(space + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        values[key] = value;
    }
    return values;
}

/// The character's CVars as its config-cache.wtf has them, the others on their
/// default: those of the one before are destroyed as it leaves the world
/// (0x00767350) and registered again as the next one enters it.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> characterCVars(const std::string& text) {
    const auto kept = configCacheValues(text);
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string_view name : clientCVarNames()) {
        const auto flags = clientCVarFlags(name);
        if (!flags || (*flags & kCVarStoredWithCharacter) == 0) continue;
        std::string key(name);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (const auto it = kept.find(key); it != kept.end()) out.emplace_back(key, it->second);
        else if (const auto def = clientCVarDefault(name)) out.emplace_back(key, std::string(*def));
    }
    return out;
}

/// Where a character's is kept here, under the configuration folder.
[[nodiscard]] inline std::string characterConfigCacheFile(const std::string& configRoot, const std::string& character) {
    return character.empty() ? std::string() : configRoot + "/characters/" + character + ".config-cache.wtf";
}

}  // namespace wowee::addons
