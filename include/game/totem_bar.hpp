#pragma once

/// The totem bar (the multi-cast action bar): what the client keeps for it
/// and how GetMultiCastTotemSpells and SetMultiCastSpell read it.
///
/// - Four element lists (DAT_00be8ea0), in the order fire, earth, water, air,
///   filled by the spellbook's add (0x00542030) with every spell that
///   summons a totem (AttributesEx7 0x20): each of its two
///   RequiredTotemCategoryID names the elements it goes to (0x005a7b50).
///   A list holds one spell of a name, its highest rank.
/// - Its action slots are 133..144 (0-based 0x84..0x8f), four to a page in
///   the same element order; a slot takes a spell of its element's totem
///   category (0x005a7ae0, 0x005a7bc0).

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee::game::totem_bar {

inline constexpr int kElements = 4;          ///< fire, earth, water, air
inline constexpr int kFirstSlot = 0x84;      ///< 0-based action slot
inline constexpr int kLastSlot = 0x8f;

/// 0x005a7b50: the elements a TotemCategory belongs to, as a bit per list.
[[nodiscard]] constexpr uint32_t elementMask(uint32_t totemCategory) {
    switch (totemCategory) {
        case 2: return 0x2;    // earth
        case 3: return 0x8;    // air
        case 4: return 0x1;    // fire
        case 5: return 0x4;    // water
        case 21: return 0xf;   // all four
        default: return 0;
    }
}

/// 0x005a7ae0: the TotemCategory a 0-based action slot of the bar takes, 0
/// for a slot not on it.
[[nodiscard]] constexpr uint32_t slotCategory(int slot) {
    if (slot < kFirstSlot || slot > kLastSlot) return 0;
    constexpr uint32_t kByColumn[4] = {4, 2, 5, 3};
    return kByColumn[(slot - kFirstSlot) % 4];
}

/// 0x005a7bc0: whether the spell's categories hold the slot's.
[[nodiscard]] constexpr bool slotTakes(int slot, const uint32_t (&spellCategories)[2]) {
    const uint32_t want = slotCategory(slot);
    return want != 0 && (spellCategories[0] == want || spellCategories[1] == want);
}

struct Spell {
    uint32_t id = 0;
    std::string name;
    uint32_t rank = 0;   ///< 0x0053b540, else the spell's own level; 0 for none
    uint32_t categories[2] = {0, 0};
};

using Lists = std::array<std::vector<uint32_t>, kElements>;

/// 0x00542030's totem branch for one spell: into each of its elements' lists
/// once - in place of a lower rank of the same name, after the rest for a
/// new name or one with no rank. `nameOf` and `rankOf` answer for a spell
/// already listed.
inline void add(Lists& lists, const Spell& spell, const std::function<std::string(uint32_t)>& nameOf,
                const std::function<uint32_t(uint32_t)>& rankOf) {
    for (uint32_t category : spell.categories) {
        const uint32_t mask = elementMask(category);
        for (int e = 0; e < kElements; ++e) {
            if ((mask & (1u << e)) == 0) continue;
            auto& list = lists[static_cast<size_t>(e)];
            bool present = false;
            for (uint32_t id : list) present = present || id == spell.id;
            if (present) continue;
            if (spell.rank == 0) {
                list.push_back(spell.id);
                continue;
            }
            bool sameName = false;
            for (uint32_t& id : list) {
                if (nameOf(id) != spell.name) continue;
                sameName = true;
                if (rankOf(id) < spell.rank) id = spell.id;
                break;
            }
            if (!sameName) list.push_back(spell.id);
        }
    }
}

}  // namespace wowee::game::totem_bar
