#pragma once

/// Override bindings: what SetOverrideBinding and its Spell, Item, Macro and
/// Click forms put over a key (0x00563480 -> 0x005603d0), and what
/// SetMouselookOverrideBinding puts over one while the mouse looks
/// (0x005fd440).
///
/// A key holds a list of overrides, each owned by a frame: setting one for an
/// owner replaces that owner's, a nil command takes it away, and
/// ClearOverrideBindings takes all of an owner's away (0x005604f0). The key
/// then answers to its priority overrides ahead of the others, and among
/// those to the one set last; with none, to its own binding
/// (GetBindingAction with checkOverride, 0x00562490).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wowee::addons {

class OverrideBindings {
public:
    /// The binding tables are keyed without regard to case (0x0076f640).
    static std::string normalize(std::string key) {
        for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return key;
    }

    void set(const void* owner, bool priority, const std::string& key, const std::string& command) {
        const std::string k = normalize(key);
        auto& list = byKey_[k];
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Entry& e) { return e.owner == owner; }),
                   list.end());
        if (!command.empty()) list.push_back({owner, priority, command, ++sequence_});
        if (list.empty()) byKey_.erase(k);
    }

    void clear(const void* owner) {
        for (auto it = byKey_.begin(); it != byKey_.end();) {
            auto& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Entry& e) { return e.owner == owner; }),
                       list.end());
            it = list.empty() ? byKey_.erase(it) : std::next(it);
        }
    }

    /// The command the key's overrides give it, if any.
    [[nodiscard]] std::optional<std::string> lookup(const std::string& key) const {
        const auto it = byKey_.find(normalize(key));
        if (it == byKey_.end()) return std::nullopt;
        const Entry* best = nullptr;
        for (const Entry& e : it->second) {
            if (!best || (e.priority && !best->priority) ||
                (e.priority == best->priority && e.sequence > best->sequence))
                best = &e;
        }
        if (!best) return std::nullopt;
        return best->command;
    }

    /// SetMouselookOverrideBinding: one binding a key takes while the mouse
    /// looks; nil takes it away.
    void setMouselook(const std::string& key, const std::string& binding) {
        const std::string k = normalize(key);
        if (binding.empty()) mouselook_.erase(k);
        else mouselook_[k] = binding;
    }
    [[nodiscard]] std::optional<std::string> mouselook(const std::string& key) const {
        const auto it = mouselook_.find(normalize(key));
        if (it == mouselook_.end()) return std::nullopt;
        return it->second;
    }

    void reset() {
        byKey_.clear();
        mouselook_.clear();
    }

private:
    struct Entry {
        const void* owner = nullptr;
        bool priority = false;
        std::string command;
        uint64_t sequence = 0;
    };
    std::map<std::string, std::vector<Entry>> byKey_;
    std::map<std::string, std::string> mouselook_;
    uint64_t sequence_ = 0;
};

/// The one store the bindings functions and the key dispatch share.
OverrideBindings& overrideBindings();

}  // namespace wowee::addons
