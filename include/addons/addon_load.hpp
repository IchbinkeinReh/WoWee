#pragma once

/// Which addons are loaded, and in what order (0x005f7e90, 0x005f80b0,
/// 0x005f84a0).

#include <string>
#include <vector>

namespace wowee::addons::addon_load {

/// What is known of one addon. Its own names are matched by the caller.
struct Addon {
    std::string name;
    bool enabled = true;
    bool banned = false;
    bool onDemand = false;
    std::vector<std::string> optional;   ///< OptionalDeps: loaded before it, where they can be
    std::vector<std::string> required;   ///< not loaded without these
    std::vector<std::string> managers;   ///< LoadManagers: with one of them, it waits for it
    std::vector<std::string> withIt;     ///< those whose LoadWith names it
};

/// What is asked of the addons there are.
struct World {
    virtual ~World() = default;
    /// The addon of that name, in any case; null for none.
    virtual const Addon* find(const std::string& name) = 0;
    virtual bool loaded(const Addon& addon) = 0;
    /// Begun: loaded from here on (+0x18), or not after all.
    virtual void begin(const Addon& addon, bool on) = 0;
    /// Its files, its bindings and what it saved (0x005f80b0's middle).
    virtual void run(const Addon& addon) = 0;
};

/// Why an addon is not loaded, "" where it may be: its own reason, or a
/// required one's with "DEP_" before it. `asked`: by LoadAddOn or by what
/// requires it, not with the interface.
inline std::string refusal(World& world, const std::string& name, bool asked, std::vector<const Addon*> path = {}) {
    const Addon* addon = world.find(name);
    if (!addon) return "MISSING";
    // On the way to itself.
    for (const Addon* on : path)
        if (on == addon) return "";
    if (!addon->enabled) return "DISABLED";
    if (addon->banned) return "BANNED";
    path.push_back(addon);
    for (const std::string& needed : addon->required) {
        const Addon* dep = world.find(needed);
        if (dep && world.loaded(*dep)) continue;
        const std::string why = refusal(world, needed, true, path);
        if (!why.empty()) return why.rfind("DEP_", 0) == 0 ? why : "DEP_" + why;
    }
    return !asked && addon->onDemand ? "DEMAND_LOADED" : "";
}

/// An addon, loaded: first the optional ones, then those it requires, then its
/// own files, then those that go with it. False where it may not be, or one it
/// requires came to nothing.
inline bool load(World& world, const std::string& name, bool asked) {
    const Addon* addon = world.find(name);
    if (!addon) return false;
    if (world.loaded(*addon)) return true;
    if (!refusal(world, name, asked).empty()) return false;
    world.begin(*addon, true);
    for (const std::string& other : addon->optional) load(world, other, true);
    for (const std::string& needed : addon->required) {
        if (load(world, needed, true)) continue;
        world.begin(*addon, false);
        return false;
    }
    world.run(*addon);
    for (const std::string& other : addon->withIt) load(world, other, asked);
    return true;
}

/// All of them, as the interface is loaded: one with a manager that loads waits
/// for it.
inline void loadAll(World& world, const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        const Addon* addon = world.find(name);
        if (!addon) continue;
        bool managed = false;
        for (const std::string& manager : addon->managers) {
            managed = load(world, manager, false);
            if (managed) break;
        }
        if (!managed) load(world, name, false);
    }
}

}  // namespace wowee::addons::addon_load
