#pragma once

/// The equipment manager's own sets (0x00acfc08's list, ten at the most): how
/// one is saved, renamed and deleted, what the server's answer does to it, and
/// what CMSG_EQUIPMENT_SET_USE says for each slot (0x005ada20).

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::game::equipment_sets {

inline constexpr size_t kSlots = 19;
inline constexpr size_t kMaxSets = 10;
/// What a slot the set leaves alone holds in place of an item's guid.
inline constexpr uint64_t kIgnored = 1;

struct Set {
    uint64_t setGuid = 0;
    uint32_t setId = 0;   ///< its place among the ten
    std::string name;
    std::string iconName;
    std::array<uint64_t, kSlots> items{};
    /// Saved as a new set and not answered yet (+0x230): not saved again.
    bool saving = false;
};

[[nodiscard]] inline uint32_t ignoreMask(const Set& set) {
    uint32_t mask = 0;
    for (size_t i = 0; i < kSlots; ++i)
        if (set.items[i] == kIgnored) mask |= 1u << i;
    return mask;
}

[[nodiscard]] inline Set* byName(std::vector<Set>& sets, const std::string& name) {
    for (auto& set : sets)
        if (set.name == name) return &set;
    return nullptr;
}
[[nodiscard]] inline Set* byId(std::vector<Set>& sets, uint32_t id) {
    for (auto& set : sets)
        if (set.setId == id) return &set;
    return nullptr;
}

/// The lowest place no set has (0x005ae530), or none.
[[nodiscard]] inline std::optional<uint32_t> freeId(const std::vector<Set>& sets) {
    for (uint32_t id = 0; id < kMaxSets; ++id) {
        if (std::none_of(sets.begin(), sets.end(), [id](const Set& s) { return s.setId == id; })) return id;
    }
    return std::nullopt;
}

using Worn = std::array<uint64_t, kSlots>;

/// What is worn now, the marked slots left out (0x005ade50).
[[nodiscard]] inline std::array<uint64_t, kSlots> itemsOf(const Worn& worn, uint32_t ignored) {
    std::array<uint64_t, kSlots> items{};
    for (size_t i = 0; i < kSlots; ++i) items[i] = (ignored & (1u << i)) != 0 ? kIgnored : worn[i];
    return items;
}

/// The set of that name with what is worn now, a new one if there is none
/// (0x005af8e0, 0x005af7e0). The set to send, or none: its first save is not
/// answered yet, or the ten are taken.
inline Set* save(std::vector<Set>& sets, const std::string& name, const std::string& icon, const Worn& worn,
                 uint32_t ignored) {
    if (Set* set = byName(sets, name)) {
        if (set->saving) return nullptr;
        set->iconName = icon;
        set->items = itemsOf(worn, ignored);
        return set;
    }
    const auto id = sets.size() < kMaxSets ? freeId(sets) : std::nullopt;
    if (!id) return nullptr;
    sets.push_back({0, *id, name, icon, itemsOf(worn, ignored), true});
    return &sets.back();
}

struct Renamed {
    bool changed = false;   ///< EQUIPMENT_SETS_CHANGED is raised
    Set* send = nullptr;    ///< the set to send
};

/// A set given a name no set has yet, and saved again with what is worn now
/// (0x005af400 -> 0x005ade50) - but for one not answered yet, which is named
/// and not sent.
inline Renamed rename(std::vector<Set>& sets, const std::string& from, const std::string& to, const Worn& worn,
                      uint32_t ignored) {
    Set* set = byName(sets, from);
    if (!set || byName(sets, to)) return {};
    set->name = to;
    if (set->saving) return {true, nullptr};
    set->items = itemsOf(worn, ignored);
    return {true, set};
}

/// SMSG_EQUIPMENT_SET_SAVED (0x005ae760): the place's set has that guid and
/// may be saved again; no guid means the server has not kept it, and it goes.
inline void saved(std::vector<Set>& sets, uint32_t id, uint64_t guid) {
    if (guid == 0) {
        std::erase_if(sets, [id](const Set& s) { return s.setId == id; });
        return;
    }
    if (Set* set = byId(sets, id)) {
        set->setGuid = guid;
        set->saving = false;
    }
}

/// Where an item is: the bag and the slot as the wire names them, 255 for the
/// player's own slots.
struct Place {
    uint8_t bag = 0;
    uint8_t slot = 0;
};

struct SwapSlot {
    uint64_t guid = kIgnored;
    Place from;
};

struct Swap {
    std::array<SwapSlot, kSlots> slots;
    bool missing = false;   ///< EQUIPMENT_MANAGER_MISSING_ITEM is said
    bool combat = false;    ///< EQUIPMENT_MANAGER_COMBAT_SWAP is said
};

/// What CMSG_EQUIPMENT_SET_USE says (0x005ada20): an item where it is, 0 to
/// take off what is worn, 1 to leave the slot alone - as for an item that is
/// nowhere, and in combat for every slot but the weapons'. `find` answers
/// where an item is carried, the bank counting while it is open.
inline Swap swap(const std::array<uint64_t, kSlots>& items, const Worn& worn, bool inCombat,
                 const std::function<std::optional<Place>(uint64_t)>& find) {
    Swap out;
    for (size_t i = 0; i < kSlots; ++i) {
        const bool weapon = i == 15 || i == 16 || i == 17;
        if (inCombat && !weapon) {
            if (items[i] != kIgnored && items[i] != worn[i]) out.combat = true;
            continue;
        }
        if (items[i] == kIgnored) continue;
        if (items[i] == 0) {
            out.slots[i].guid = 0;
            continue;
        }
        if (const auto place = find ? find(items[i]) : std::nullopt) out.slots[i] = {items[i], *place};
        else out.missing = true;
    }
    return out;
}

}  // namespace wowee::game::equipment_sets
