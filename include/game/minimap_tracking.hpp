#pragma once

// The minimap tracking menu's own entries, beside the tracking spells: the
// town services, the mailbox and low-level quests (0x00a11c50, fifteen of
// them, offered by 0x0057eb00 to the classes each names). One may be chosen
// at a time; the minimap marks what it finds with ObjectIcons' cell 8
// (0x0057f7f0), and the choice is kept in the minimapTrackedInfo CVar by name
// (0x0057e070).

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wowee {
namespace game {
namespace minimap_tracking {

enum class Kind : uint32_t {
    NpcFlag = 1,         ///< creatures with any of these UNIT_NPC_FLAGS
    GameObjectType = 2,  ///< game objects of this type
    TrivialQuests = 3,   ///< quest givers whose quests are below the player
};

struct Other {
    Kind kind;
    uint32_t mask;
    const char* name;   ///< the GlobalStrings key, also what the CVar keeps
    const char* icon;   ///< under Interface\Minimap\Tracking
    uint32_t classes;   ///< 1 << class for each class offered it, 0 for all
};

inline constexpr std::array<Other, 15> kOther = {{
    {Kind::NpcFlag, 0x1000, "MINIMAP_TRACKING_REPAIR", "Repair", 0},
    {Kind::NpcFlag, 0x200, "MINIMAP_TRACKING_VENDOR_FOOD", "Food", 0},
    {Kind::NpcFlag, 0x400, "MINIMAP_TRACKING_VENDOR_POISON", "Poisons", 0x10},
    {Kind::NpcFlag, 0x100, "MINIMAP_TRACKING_VENDOR_AMMO", "Ammunition", 0x1a},
    {Kind::NpcFlag, 0x800, "MINIMAP_TRACKING_VENDOR_REAGENT", "Reagents", 0},
    {Kind::NpcFlag, 0x10000, "MINIMAP_TRACKING_INNKEEPER", "Innkeeper", 0},
    {Kind::NpcFlag, 0x2000, "MINIMAP_TRACKING_FLIGHTMASTER", "FlightMaster", 0},
    {Kind::NpcFlag, 0x400000, "MINIMAP_TRACKING_STABLEMASTER", "StableMaster", 0x8},
    {Kind::NpcFlag, 0x100000, "MINIMAP_TRACKING_BATTLEMASTER", "BattleMaster", 0},
    {Kind::NpcFlag, 0x20, "MINIMAP_TRACKING_TRAINER_CLASS", "Class", 0},
    {Kind::NpcFlag, 0x40, "MINIMAP_TRACKING_TRAINER_PROFESSION", "Profession", 0},
    {Kind::NpcFlag, 0x200000, "MINIMAP_TRACKING_AUCTIONEER", "Auctioneer", 0},
    {Kind::NpcFlag, 0x20000, "MINIMAP_TRACKING_BANKER", "Banker", 0},
    {Kind::GameObjectType, 0x13, "MINIMAP_TRACKING_MAILBOX", "Mailbox", 0},
    {Kind::TrivialQuests, 0, "MINIMAP_TRACKING_TRIVIAL_QUESTS", "TrivialQuests", 0},
}};

/// Whether a class (1 warrior .. 11 druid) is offered an entry (0x0057eb00
/// tests 1 << UNIT_FIELD_BYTES_0's class byte).
constexpr bool offeredTo(const Other& o, uint8_t classId) {
    return o.classes == 0 || (o.classes & (1u << (classId & 31u))) != 0;
}

/// The entries a class is offered, in the menu's order: indices into kOther.
inline std::vector<int> offered(uint8_t classId) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(kOther.size()); ++i) {
        if (offeredTo(kOther[static_cast<size_t>(i)], classId)) out.push_back(i);
    }
    return out;
}

/// The entry the CVar names, -1 for none.
inline int byName(const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < static_cast<int>(kOther.size()); ++i) {
        if (std::strcmp(kOther[static_cast<size_t>(i)].name, name) == 0) return i;
    }
    return -1;
}

}  // namespace minimap_tracking
}  // namespace game
}  // namespace wowee
