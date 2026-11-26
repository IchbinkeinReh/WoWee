// auction_filters.hpp - the auction house's category tree, in one place.
//
// GetAuctionItemClasses, GetAuctionItemSubClasses and GetAuctionInvTypes fill
// the browse tab's filter tree, and the *positions* they answer are handed
// back to QueryAuctionItems - so the lists and the lookup have to be the same
// walk, or a category searches another one's items and nothing says so.
//
// Every index FrameXML hands back is 1-based and nil when nothing is picked,
// which arrives as zero - so zero reads as "any" without a special case.
// Before this, nil went to the wire as zero, which is a real item class:
// searching the auction house by name alone returned consumables only.
//
// The three tiers follow the real client (0x0059c420, 0x0059c480, 0x0059c590,
// QueryAuctionItems 0x0059bcf0): twelve classes, every visible ItemSubClass.dbc
// row of a class in record order, and a fourteen-row slot tier under armour.
// The English labels here are only for a client without the DBC files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace wowee {
namespace game {

/// 0xFFFFFFFF is what CMSG_AUCTION_LIST_ITEMS means by "do not filter on this".
/// AzerothCore's searcher compares `proto->Class != itemClass` for anything
/// else, so any other value is a filter - including zero.
inline constexpr uint32_t kAuctionAny = 0xFFFFFFFFu;

/// One of the twelve categories, as the client's own table at 0x00a14f70
/// holds it (eight bytes a row): an item class id and whether the class is
/// divided further. GetAuctionItemSubClasses (0x0059c480) answers nothing for
/// a class whose flag is clear, which is Quest alone.
struct AuctionClassFilter { const char* label; uint32_t classId; bool hasSubClasses; };

// WoW 3.3.5a item class ids: 0=Consumable, 1=Container, 2=Weapon, 3=Gem,
// 4=Armor, 6=Projectile, 7=Trade Goods, 9=Recipe, 11=Quiver, 12=Quest,
// 15=Miscellaneous, 16=Glyph.
//
// Twelve categories in the real client's order, and the order matters outside
// this file: GetAuctionItemClasses hands the list to Lua positionally, and
// addons read positions out of it rather than names - Bagnon's item component
// opens with `select(12, GetAuctionItemClasses())` to get "Quest".
//
// Row zero is "All" and is not the client's; it is what a nil index lands on.
inline constexpr AuctionClassFilter kAuctionClasses[] = {
    {.label = "All",           .classId = kAuctionAny, .hasSubClasses = false},
    {.label = "Weapon",        .classId = 2,  .hasSubClasses = true},
    {.label = "Armor",         .classId = 4,  .hasSubClasses = true},
    {.label = "Container",     .classId = 1,  .hasSubClasses = true},
    {.label = "Consumable",    .classId = 0,  .hasSubClasses = true},
    {.label = "Glyph",         .classId = 16, .hasSubClasses = true},
    {.label = "Trade Goods",   .classId = 7,  .hasSubClasses = true},
    {.label = "Projectile",    .classId = 6,  .hasSubClasses = true},
    {.label = "Quiver",        .classId = 11, .hasSubClasses = true},
    {.label = "Recipe",        .classId = 9,  .hasSubClasses = true},
    {.label = "Gem",           .classId = 3,  .hasSubClasses = true},
    {.label = "Miscellaneous", .classId = 15, .hasSubClasses = true},
    {.label = "Quest",         .classId = 12, .hasSubClasses = false},
};
inline constexpr int kNumAuctionClasses = 13;

/// The class a 1-based category index stands for, or null outside the twelve
/// (0x0059c480 and 0x0059bcf0 both test `index - 1 < 12`).
inline const AuctionClassFilter* auctionClassAt(int classIndex) {
    if (classIndex < 1 || classIndex >= kNumAuctionClasses) return nullptr;
    return &kAuctionClasses[classIndex];
}

/// One row of ItemSubClass.dbc, as far as the auction house reads it: class
/// and subclass ids (fields 0 and 1), flags (field 4; 0x200 means the
/// subclass has a slot tier) and display flags (field 5; 0x2 hides the row).
/// `label` is what the row is listed as: the VerboseName, else the
/// DisplayName (0x0059c480).
struct AuctionSubClassRow {
    uint32_t classId;
    uint32_t subId;
    uint32_t flags;
    uint32_t displayFlags;
    const char* label;
};

inline constexpr uint32_t kAuctionSubClassHidden   = 0x2;    // displayFlags
inline constexpr uint32_t kAuctionSubClassHasSlots = 0x200;  // flags

/// ItemSubClass.dbc of 3.3.5a 12340 in its own record order, for the classes
/// the auction house lists. Only a fallback for a client without the file:
/// the interface reads the file, because the subclass a row stands for is its
/// *position* among the class's visible rows in record order, not its id.
/// Consumables run 5, 1, 2, 3, 7, 6, 4, 8 in the file and Trade Goods
/// 10, 5, 6, 7, ... - a list sorted by id would search Potions for the row
/// that says Food & Drink.
inline constexpr AuctionSubClassRow kItemSubClassFallback[] = {
    {0, 0, 0, 3, "Consumable"}, {0, 5, 0, 1, "Food & Drink"}, {0, 1, 0, 1, "Potion"},
    {0, 2, 0, 1, "Elixir"}, {0, 3, 0, 1, "Flask"}, {0, 7, 0, 1, "Bandage"},
    {0, 6, 0, 1, "Item Enhancement"}, {0, 4, 0, 1, "Scroll"}, {0, 8, 0, 1, "Other"},
    {1, 0, 0, 0, "Bag"}, {1, 1, 0, 4, "Soul Bag"}, {1, 2, 0, 0, "Herb Bag"},
    {1, 3, 0, 0, "Enchanting Bag"}, {1, 4, 0, 0, "Engineering Bag"}, {1, 5, 0, 0, "Gem Bag"},
    {1, 6, 0, 0, "Mining Bag"}, {1, 7, 0, 0, "Leatherworking Bag"}, {1, 8, 0, 0, "Inscription Bag"},
    {2, 0, 0x3, 0, "One-Handed Axes"}, {2, 1, 0x3, 0, "Two-Handed Axes"}, {2, 2, 0x80, 0, "Bows"},
    {2, 3, 0xa8, 0, "Guns"}, {2, 4, 0x3, 0, "One-Handed Maces"}, {2, 5, 0x3, 0, "Two-Handed Maces"},
    {2, 6, 0x3, 0, "Polearms"}, {2, 7, 0x3, 0, "One-Handed Swords"}, {2, 8, 0x3, 0, "Two-Handed Swords"},
    {2, 9, 0, 3, "Obsolete"}, {2, 10, 0x3, 0, "Staves"}, {2, 11, 0, 3, "One-Handed Exotics"},
    {2, 12, 0, 3, "Two-Handed Exotics"}, {2, 13, 0x5, 0, "Fist Weapons"}, {2, 14, 0x2, 1, "Miscellaneous"},
    {2, 15, 0x3, 0, "Daggers"}, {2, 16, 0x10, 0, "Thrown"}, {2, 17, 0x3, 2, "Spears"},
    {2, 18, 0xa0, 0, "Crossbows"}, {2, 19, 0x100, 0, "Wands"}, {2, 20, 0, 0, "Fishing Poles"},
    {3, 0, 0, 1, "Red"}, {3, 1, 0, 1, "Blue"}, {3, 2, 0, 1, "Yellow"}, {3, 3, 0, 1, "Purple"},
    {3, 4, 0, 1, "Green"}, {3, 5, 0, 1, "Orange"}, {3, 6, 0, 1, "Meta"}, {3, 7, 0, 1, "Simple"},
    {3, 8, 0, 1, "Prismatic"},
    {4, 0, 0x200, 1, "Miscellaneous"}, {4, 1, 0x200, 0, "Cloth"}, {4, 2, 0x200, 0, "Leather"},
    {4, 3, 0x200, 0, "Mail"}, {4, 4, 0x200, 0, "Plate"}, {4, 5, 0, 2, "Bucklers"},
    {4, 6, 0, 0, "Shields"}, {4, 7, 0, 0, "Librams"}, {4, 8, 0, 0, "Idols"},
    {4, 9, 0, 0, "Totems"}, {4, 10, 0, 0, "Sigils"},
    {6, 0, 0, 3, "Wand"}, {6, 1, 0, 2, "Bolt"}, {6, 2, 0, 0, "Arrow"}, {6, 3, 0, 0, "Bullet"},
    {6, 4, 0x40, 2, "Thrown"},
    {7, 0, 0, 3, "Trade Goods"}, {7, 10, 0, 1, "Elemental"}, {7, 5, 0, 1, "Cloth"},
    {7, 6, 0, 1, "Leather"}, {7, 7, 0, 1, "Metal & Stone"}, {7, 8, 0, 1, "Meat"},
    {7, 9, 0, 1, "Herb"}, {7, 12, 0, 1, "Enchanting"}, {7, 4, 0, 1, "Jewelcrafting"},
    {7, 1, 0, 1, "Parts"}, {7, 3, 0, 1, "Devices"}, {7, 2, 0, 1, "Explosives"},
    {7, 13, 0, 1, "Materials"}, {7, 11, 0, 1, "Other"}, {7, 14, 0, 1, "Armor Enchantment"},
    {7, 15, 0, 1, "Weapon Enchantment"},
    {9, 0, 0, 1, "Book"}, {9, 1, 0, 1, "Leatherworking"}, {9, 2, 0, 1, "Tailoring"},
    {9, 3, 0, 1, "Engineering"}, {9, 4, 0, 1, "Blacksmithing"}, {9, 5, 0, 1, "Cooking"},
    {9, 6, 0, 1, "Alchemy"}, {9, 7, 0, 1, "First Aid"}, {9, 8, 0, 1, "Enchanting"},
    {9, 9, 0, 1, "Fishing"}, {9, 10, 0, 1, "Jewelcrafting"}, {9, 11, 0, 1, "Inscription"},
    {11, 0, 0, 7, "Quiver"}, {11, 1, 0, 7, "Quiver"}, {11, 2, 0, 4, "Quiver"},
    {11, 3, 0, 4, "Ammo Pouch"},
    {12, 0, 0, 1, "Quest"},
    {15, 0, 0, 1, "Junk"}, {15, 1, 0, 1, "Reagent"}, {15, 2, 0, 1, "Pet"},
    {15, 3, 0, 1, "Holiday"}, {15, 4, 0, 1, "Other"}, {15, 5, 0, 0, "Mount"},
    {16, 1, 0, 0, "Warrior"}, {16, 2, 0, 0, "Paladin"}, {16, 3, 0, 0, "Hunter"},
    {16, 4, 0, 0, "Rogue"}, {16, 5, 0, 0, "Priest"}, {16, 6, 0, 0, "Death Knight"},
    {16, 7, 0, 0, "Shaman"}, {16, 8, 0, 0, "Mage"}, {16, 9, 0, 0, "Warlock"},
    {16, 11, 0, 0, "Druid"},
};

/// The subclass rows listed under a class, in listing order: the class's rows
/// in record order, less those whose display flags hide them (0x0059c480).
/// Element n is the subclass index n + 1 that FrameXML hands back, and
/// QueryAuctionItems resolves that index by the same walk (0x0059bcf0).
template <typename Row>
inline std::vector<const Row*> auctionSubClassRows(const Row* rows, size_t n, uint32_t classId) {
    std::vector<const Row*> out;
    for (size_t i = 0; i < n; ++i) {
        if (rows[i].classId == classId && (rows[i].displayFlags & kAuctionSubClassHidden) == 0) {
            out.push_back(&rows[i]);
        }
    }
    return out;
}

/// The auction house's slot tier: fourteen rows (0x00a14fd0, 0x2c bytes a
/// row: the inventory type, a 32-byte global string name, two flags). It has
/// no weapon slots - the tier exists only under a subclass flagged 0x200,
/// which in 3.3.5a is armour's first five.
///
/// `misc` offers the row under armour's first listed subclass (Miscellaneous);
/// `jewellery` hides it under every other one, except that the second (Cloth)
/// offers the back - which is how 0x0059c590 reads the two flags.
struct AuctionInvTypeFilter { uint32_t invType; const char* token; bool misc; bool jewellery; };
inline constexpr AuctionInvTypeFilter kAuctionInvTypes[] = {
    {1,  "INVTYPE_HEAD",     true,  false},
    {2,  "INVTYPE_NECK",     true,  true},
    {3,  "INVTYPE_SHOULDER", false, false},
    {4,  "INVTYPE_BODY",     true,  true},
    {5,  "INVTYPE_CHEST",    false, false},
    {6,  "INVTYPE_WAIST",    false, false},
    {7,  "INVTYPE_LEGS",     false, false},
    {8,  "INVTYPE_FEET",     false, false},
    {9,  "INVTYPE_WRIST",    false, false},
    {10, "INVTYPE_HAND",     false, false},
    {11, "INVTYPE_FINGER",   true,  true},
    {12, "INVTYPE_TRINKET",  true,  true},
    {16, "INVTYPE_CLOAK",    false, true},
    {23, "INVTYPE_HOLDABLE", true,  true},
};
inline constexpr int kNumAuctionInvTypes = 14;

/// Whether GetAuctionInvTypes offers slot row `row` (0-based) under a class
/// and the 0-based position of its listed subclass (0x0059c590): under armour
/// the first subclass offers the misc rows and the others every row that is
/// not jewellery, the second one the back as well; under any other class
/// every row.
inline bool auctionInvTypeOffered(uint32_t classId, int subPosition, int row) {
    if (row < 0 || row >= kNumAuctionInvTypes) return false;
    const auto& t = kAuctionInvTypes[row];
    if (classId != 4) return true;
    if (subPosition == 0) return t.misc;
    if (subPosition == 1 && t.invType == 16) return true;
    return !t.jewellery;
}

/// The inventory type QueryAuctionItems sends for the slot index FrameXML
/// hands back: a 1-based row of all fourteen, whatever the class
/// (0x0059bcf0 reads `table[index - 1]` for `index - 1 < 14`). FrameXML
/// numbers the rows by their pair position in GetAuctionInvTypes' answer,
/// hidden rows included, so the two agree without a subset to re-map.
inline uint32_t auctionInvTypeIdAt(int index) {
    if (index < 1 || index > kNumAuctionInvTypes) return kAuctionAny;
    return kAuctionInvTypes[index - 1].invType;
}

/// Equipment-slot labels by inventory type, for the trade skill window's slot
/// filter (not the auction house's slot tier, which is kAuctionInvTypes).
struct AuctionSlotFilter { const char* label; uint32_t invType; const char* token; };
inline constexpr AuctionSlotFilter kAuctionSlots[] = {
    {.label = "All Slots",     .invType = kAuctionAny, .token = ""},
    {.label = "Head",          .invType = 1,  .token = "INVTYPE_HEAD"},
    {.label = "Neck",          .invType = 2,  .token = "INVTYPE_NECK"},
    {.label = "Shoulder",      .invType = 3,  .token = "INVTYPE_SHOULDER"},
    {.label = "Chest",         .invType = 5,  .token = "INVTYPE_CHEST"},
    {.label = "Waist",         .invType = 6,  .token = "INVTYPE_WAIST"},
    {.label = "Legs",          .invType = 7,  .token = "INVTYPE_LEGS"},
    {.label = "Feet",          .invType = 8,  .token = "INVTYPE_FEET"},
    {.label = "Wrist",         .invType = 9,  .token = "INVTYPE_WRIST"},
    {.label = "Hands",         .invType = 10, .token = "INVTYPE_HAND"},
    {.label = "Finger",        .invType = 11, .token = "INVTYPE_FINGER"},
    {.label = "Trinket",       .invType = 12, .token = "INVTYPE_TRINKET"},
    {.label = "Back",          .invType = 16, .token = "INVTYPE_CLOAK"},
    {.label = "One-Hand",      .invType = 13, .token = "INVTYPE_WEAPON"},
    {.label = "Two-Hand",      .invType = 17, .token = "INVTYPE_2HWEAPON"},
    {.label = "Main Hand",     .invType = 21, .token = "INVTYPE_WEAPONMAINHAND"},
    {.label = "Off Hand",      .invType = 22, .token = "INVTYPE_WEAPONOFFHAND"},
    {.label = "Ranged",        .invType = 26, .token = "INVTYPE_RANGED"},
    {.label = "Shield",        .invType = 14, .token = "INVTYPE_SHIELD"},
    {.label = "Held Off-hand", .invType = 23, .token = "INVTYPE_HOLDABLE"},
    {.label = "Relic",         .invType = 28, .token = "INVTYPE_RELIC"},
};
inline constexpr int kNumAuctionSlots = 21;

/// The three lengths an auction may run for, in minutes, which is what the
/// wire field carries: AzerothCore multiplies it by sixty and then accepts
/// only 1, 2 or 4 times MIN_AUCTION_TIME (12 hours), rejecting the request
/// outright otherwise. FrameXML's duration dropdown does not deal in minutes -
/// its values are 1, 2 and 3 for the same three lengths - so a posting made
/// through it asked for one, two or three minutes and the server dropped it
/// without a word.
inline constexpr uint32_t kAuctionDurationMinutes[3] = {720, 1440, 2880};

/// Minutes for StartAuction's runTime (0x0059f990): 1, 2 and 3 are the
/// table's (0x00a15248), anything else twenty-four hours (0x5a0).
inline constexpr uint32_t auctionDurationMinutes(uint32_t d) {
    if (d >= 1 && d <= 3) return kAuctionDurationMinutes[d - 1];
    return 1440;
}

} // namespace game
} // namespace wowee
