#pragma once

// What of the settings was changed by the MCP server's doing, and what of the
// account's copy on the realm is therefore not sent up (docs/MCP_SERVER.md).
//
// A tool that clicks its way through the options changes the player's CVars,
// and a second later the client keeps them with the account on the realm: for
// good, and on every computer. What is changed is not refused - it is there to
// be tried -, but it is told (`state`), and the account's copy of that kind
// stays as it is until the client ends, unless --mcp-allow=settings.

#include <array>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wowee::mcp {

/// The kinds of account data that are settings: the CVars (0, 1), the key
/// bindings (2, 3) and the chat windows (7); the macros (4, 5) are not.
[[nodiscard]] constexpr bool isSettingsData(uint32_t type) { return type <= 3 || type == 7; }
[[nodiscard]] constexpr const char* settingsDataName(uint32_t type) {
    switch (type) {
        case 0: return "the account's CVars";
        case 1: return "the character's CVars";
        case 2: return "the account's key bindings";
        case 3: return "the character's key bindings";
        case 7: return "the chat windows";
        default: return "account data";
    }
}

/// bindings-cache.wtf's lines ("bind KEY COMMAND") that differ: key, before, after.
struct BindingChange {
    std::string key, before, after;
};
[[nodiscard]] inline std::vector<BindingChange> bindingChanges(const std::string& before, const std::string& after) {
    const auto keys = [](const std::string& text) {
        std::map<std::string, std::string> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream words(line);
            std::string verb, key, command;
            if (words >> verb >> key >> command && verb == "bind") out[key] = command;
        }
        return out;
    };
    const auto was = keys(before);
    const auto is = keys(after);
    std::vector<BindingChange> out;
    for (const auto& [key, command] : is) {
        const auto it = was.find(key);
        if (it == was.end() || it->second != command)
            out.push_back({key, it == was.end() ? std::string() : it->second, command});
    }
    for (const auto& [key, command] : was)
        if (!is.count(key)) out.push_back({key, command, {}});
    return out;
}

class SettingsLedger {
public:
    struct Change {
        std::string name, before, after, when;
    };

    /// A setting changed by the server's doing. Told of once, with what it was
    /// first and is now; not at all where that is the same again.
    void changed(std::string_view name, std::string_view before, std::string_view after, std::string when) {
        if (before == after) return;
        for (auto it = changes_.begin(); it != changes_.end(); ++it) {
            if (it->name != name) continue;
            if (it->before == after) {
                changes_.erase(it);
                return;
            }
            it->after = after;
            it->when = std::move(when);
            return;
        }
        constexpr size_t kMost = 500;
        if (changes_.size() >= kMost) return;
        changes_.push_back({std::string(name), std::string(before), std::string(after), std::move(when)});
    }
    [[nodiscard]] const std::vector<Change>& changes() const { return changes_; }

    /// Whether a kind of account data that has changed may be sent up.
    /// `byServer`: by the server's doing. Once it is not, it stays so: what a
    /// person changes after would take the server's changes along.
    bool mayGoUp(uint32_t type, bool byServer, bool allowed) {
        if (allowed || !isSettingsData(type) || type >= held_.size()) return true;
        if (byServer) held_[type] = true;
        return !held_[type];
    }
    [[nodiscard]] bool held(uint32_t type) const { return type < held_.size() && held_[type]; }
    /// Whether what a look for changes has found is not the server's doing: nothing, or CVars,
    /// whose changes are noted one by one as they are made, with none of its among them - what the
    /// client sets itself as the world comes.
    [[nodiscard]] bool notTheServers(uint32_t type, bool changed) const {
        return !changed || (type <= 1 && !held(type));
    }
    /// Once a kind.
    bool firstToldOf(uint32_t type) { return type < told_.size() && !std::exchange(told_[type], true); }

private:
    std::vector<Change> changes_;
    std::array<bool, 8> held_{};
    std::array<bool, 8> told_{};
};

}  // namespace wowee::mcp
