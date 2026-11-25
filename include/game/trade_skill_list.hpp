#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/client_compare.hpp"

// The profession window's list as the client keeps it (TradeSkillFrame.cpp): the
// recipes, one heading per item class and subclass they make something of, and the
// filters and closed headings that decide which of them are shown. Every Lua call
// that takes an index counts the rows in the order below, the hidden ones past the
// shown.

namespace wowee::game::trade_skill {

/// How hard a recipe is at a rank (0x005dd5a0): 0 optimal, 1 medium, 2 easy, 3 trivial.
/// `high` and `low` are the two ranks of its SkillLineAbility row (+0x28, +0x2c); where
/// it has no low one that is 25 under the high one.
[[nodiscard]] constexpr int difficulty(int rank, int high, int low) {
    if (low == 0) low = high - 25;
    if (rank < low) return 0;
    if (rank < (low + high) / 2) return 1;
    return rank < high ? 2 : 3;
}

/// The bits of a recipe's slots, in the order of their names (0x00ad0e10).
inline constexpr std::array<const char*, 27> kSlotNames{
    "HEADSLOT",     "NECKSLOT",     "SHOULDERSLOT", "SHIRTSLOT",    "CHESTSLOT",         "WAISTSLOT",  "LEGSSLOT",
    "FEETSLOT",     "WRISTSLOT",    "HANDSSLOT",    "FINGER0SLOT",  "FINGER1SLOT",       "TRINKET0SLOT", "TRINKET1SLOT",
    "BACKSLOT",     "MAINHANDSLOT", "SECONDARYHANDSLOT", "RANGEDSLOT", "TABARDSLOT",     "BAGSLOT",    "BAGSLOT",
    "BAGSLOT",      "BAGSLOT",      "ENCHSLOT_WEAPON", "ENCHSLOT_2HWEAPON", "SHIELDSLOT", "NONEQUIPSLOT"};
inline constexpr uint32_t kWeaponEnchant = 0x800000, kTwoHandEnchant = 0x1000000, kShieldEnchant = 0x2000000,
                          kNoSlot = 0x4000000;

/// The slots an inventory type goes to (0x00a2d288).
[[nodiscard]] constexpr uint32_t inventoryTypeSlots(uint32_t type) {
    constexpr std::array<uint32_t, 29> kSlots{0x0,    0x1,     0x2,      0x4,     0x8,    0x10,    0x20,    0x40,
                                              0x80,   0x100,   0x200,    0xc00,   0x3000, 0x18000, 0x10000, 0x20000,
                                              0x4000, 0x18000, 0x780000, 0x40000, 0x10,   0x18000, 0x18000, 0x10000,
                                              0x0,    0x20000, 0x20000,  0x0,     0x20000};
    return type < kSlots.size() ? kSlots[type] : 0;
}

/// A made item's: of the several a ring, a trinket or a bag has the first alone, and
/// that of no slot where it is worn nowhere.
[[nodiscard]] constexpr uint32_t itemSlots(uint32_t type) {
    uint32_t slots = inventoryTypeSlots(type);
    if (type == 18) slots = 0x80000;
    else if (type == 11) slots = 0x400;
    else if (type == 12) slots = 0x1000;
    return slots != 0 ? slots : kNoSlot;
}

/// Those of a recipe that makes no item, by what its spell is cast on (EquippedItemClass
/// and the masks of its subclasses and inventory types).
[[nodiscard]] constexpr uint32_t enchantSlots(int32_t itemClass, uint32_t subClasses, uint32_t inventoryTypes) {
    if (itemClass == 2) return (subClasses & 1) != 0 ? kWeaponEnchant : kTwoHandEnchant;
    if (itemClass != 4) return kNoSlot;
    if (inventoryTypes == 0) return (subClasses & 0x40) != 0 ? kShieldEnchant : kNoSlot;
    for (uint32_t type = 0; type < 29; ++type) {
        if ((inventoryTypes & (1u << type)) == 0) continue;
        uint32_t slots = inventoryTypeSlots(type);
        if ((slots & 0x80000) != 0) slots &= 0xff8fffff;
        else if ((slots & 0x400) != 0) slots &= 0xfffff7ff;
        else if ((slots & 0x1000) != 0) slots &= 0xffffdfff;
        return slots != 0 ? slots : kNoSlot;
    }
    return kNoSlot;
}

/// A recipe (0x24 bytes in the client).
struct Recipe {
    uint32_t spellId = 0;        ///< +0x00
    int difficulty = 0;          ///< +0x04
    int32_t itemClass = -1;      ///< +0x08, with the subclass -1 for one that makes no item
    int32_t itemSubClass = -1;   ///< +0x0c
    uint32_t slots = kNoSlot;    ///< +0x10
    uint32_t itemLevel = 0;      ///< +0x14
    int canMake = -1;            ///< +0x18, how many there are reagents for, -1 for one without
    int trivialAt = 0;           ///< +0x1c, its row's high rank
    std::string name;
};
/// A heading's group.
struct Group {
    int32_t itemClass = -1, itemSubClass = -1;
    std::string name;
    bool shown = true, closed = false;
    uint32_t slots = 0;
};
struct Row {
    Recipe recipe;       ///< spellId 0 for a heading
    size_t group = 0;    ///< as the groups are ordered
    bool visible = true;
    [[nodiscard]] bool header() const { return recipe.spellId == 0; }
};

class List {
public:
    struct Filters {
        uint32_t slots = ~0u;
        bool onlyMakeable = false, onlySkillUps = false;
        /// The search box's (0x005dc110); left empty, all pass.
        std::function<bool(const Recipe&)> passes;
    };
    /// A subclass's name, the skill line's verb for -1, -1.
    using NameOf = std::function<std::string(int32_t itemClass, int32_t itemSubClass)>;

    /// Another skill line's window (0x005de120): no filter and nothing closed.
    void newLine() { *this = List(); }
    /// 0x005dd5a0: the rows of these recipes. Groups that were there keep whether they
    /// are shown and closed.
    void build(std::vector<Recipe> recipes, const NameOf& nameOf, const Filters& filters) {
        const std::vector<Group> before = std::exchange(groups_, {});
        rows_.clear();
        for (Recipe& r : recipes) {
            const size_t g = find(r.itemClass, r.itemSubClass);
            if (g == groups_.size()) {
                Group group{r.itemClass, r.itemSubClass, nameOf ? nameOf(r.itemClass, r.itemSubClass) : std::string()};
                for (const Group& old : before) {
                    if (old.itemClass != group.itemClass || old.itemSubClass != group.itemSubClass) continue;
                    group.shown = old.shown;
                    group.closed = old.closed;
                }
                groups_.push_back(std::move(group));
            }
            groups_[g].slots |= r.slots;
            rows_.push_back({std::move(r), g, false});
        }
        for (size_t g = 0; g < groups_.size(); ++g) {
            Row header;
            header.recipe.itemClass = groups_[g].itemClass;
            header.recipe.itemSubClass = groups_[g].itemSubClass;
            header.group = g;
            rows_.push_back(std::move(header));
        }
        arrange(filters);
        shownMask_ = openMask_ = ~0u;
        for (size_t g = 0; g < groups_.size(); ++g) {
            if (!groups_[g].shown) shownMask_ &= ~bit(g);
            if (groups_[g].closed) openMask_ &= ~bit(g);
        }
    }
    /// 0x005dcc40(1): the filters again, the groups' by their bits.
    void refilter(const Filters& filters) {
        for (size_t g = 0; g < groups_.size(); ++g) {
            groups_[g].shown = (shownMask_ & bit(g)) != 0;
            groups_[g].closed = (openMask_ & bit(g)) == 0;
        }
        arrange(filters);
    }

    [[nodiscard]] const std::vector<Row>& rows() const { return rows_; }
    [[nodiscard]] const std::vector<Group>& groups() const { return groups_; }
    /// GetNumTradeSkills (0x005da120): the rows that show.
    [[nodiscard]] size_t visibleCount() const { return visible_; }
    [[nodiscard]] const Row* row(int index) const {
        return index >= 1 && static_cast<size_t>(index) <= rows_.size() ? &rows_[static_cast<size_t>(index) - 1] : nullptr;
    }
    /// The row of a recipe, from one, 0 where it is in none (0x005da670).
    [[nodiscard]] int indexOf(uint32_t spellId) const {
        for (size_t i = 0; spellId != 0 && i < rows_.size(); ++i)
            if (rows_[i].recipe.spellId == spellId) return static_cast<int>(i) + 1;
        return 0;
    }
    /// The first row with a recipe of those that show, 0 for none (0x005dab70).
    [[nodiscard]] int firstRecipe() const {
        for (size_t i = 0; i < visible_; ++i)
            if (!rows_[i].header()) return static_cast<int>(i) + 1;
        return 0;
    }

    /// The subclass dropdown's bits (0x005dd0f0, 0x005da1b0).
    void showAllGroups() {
        shownMask_ = ~0u;
        lastGroup_ = -1;
    }
    void showGroup(int group, bool on, bool alone) {
        const uint32_t one = bit(static_cast<size_t>(group));
        if (!on) shownMask_ &= ~one;
        else if (alone) shownMask_ = one;
        else shownMask_ |= one;
        lastGroup_ = group;
    }
    [[nodiscard]] bool groupShown(int group) const { return (shownMask_ & bit(static_cast<size_t>(group))) != 0; }
    [[nodiscard]] bool allGroupsShown() const {
        for (size_t g = 0; g < groups_.size(); ++g)
            if ((shownMask_ & bit(g)) == 0) return false;
        return true;
    }
    /// A heading closed or opened, all of them with -1 (0x005dd460, 0x005dd500).
    void close(int group, bool closed) {
        if (group < 0) openMask_ = closed ? 0u : ~0u;
        else if (closed) openMask_ &= ~bit(static_cast<size_t>(group));
        else openMask_ |= bit(static_cast<size_t>(group));
    }
    /// Whether a heading shows as open (0x005da830).
    [[nodiscard]] bool open(size_t group) const { return (openMask_ & bit(group)) != 0; }

    /// The slots the dropdown has (0x005dace0): those of all recipes, of the subclass
    /// picked last alone where one is.
    [[nodiscard]] uint32_t slots() const {
        uint32_t all = 0;
        for (const Group& g : groups_) all |= g.slots;
        return lastGroup_ >= 0 && static_cast<size_t>(lastGroup_) < groups_.size()
                   ? all & groups_[static_cast<size_t>(lastGroup_)].slots
                   : all;
    }
    /// The bit of the dropdown's `nth` entry, -1 past the last.
    [[nodiscard]] int slotBit(int nth) const {
        const uint32_t all = slots();
        for (int i = 0; i < static_cast<int>(kSlotNames.size()); ++i)
            if ((all & (1u << i)) != 0 && nth-- == 0) return i;
        return -1;
    }

private:
    static constexpr uint32_t bit(size_t i) { return 1u << (i & 0x1f); }
    [[nodiscard]] size_t find(int32_t itemClass, int32_t itemSubClass) const {
        size_t g = 0;
        while (g < groups_.size() && (groups_[g].itemClass != itemClass || groups_[g].itemSubClass != itemSubClass)) ++g;
        return g;
    }
    /// 0x005dcc40 past the bits: which rows show, and the groups and the rows in order.
    void arrange(const Filters& f) {
        std::vector<bool> any(groups_.size(), false);
        for (Row& r : rows_) {
            if (r.header()) continue;
            r.visible = groups_[r.group].shown && (r.recipe.slots & f.slots) != 0 &&
                        (!f.onlyMakeable || r.recipe.canMake != 0) && (!f.onlySkillUps || r.recipe.difficulty < 3) &&
                        (!f.passes || f.passes(r.recipe));
            if (r.visible) any[r.group] = true;
            if (groups_[r.group].closed) r.visible = false;
        }
        for (Row& r : rows_)
            if (r.header()) r.visible = any[r.group];

        // 0x005da6b0: the class and then the subclass by its name.
        std::vector<size_t> order(groups_.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) {
            if (groups_[a].itemClass != groups_[b].itemClass) return groups_[a].itemClass < groups_[b].itemClass;
            return compareCaseless(groups_[a].name, groups_[b].name) < 0;
        });
        std::vector<size_t> place(order.size());
        std::vector<Group> sorted;
        sorted.reserve(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            place[order[i]] = i;
            sorted.push_back(std::move(groups_[order[i]]));
        }
        groups_ = std::move(sorted);
        for (Row& r : rows_) r.group = place[r.group];

        // 0x005dae60: what shows first, by group, its heading before its recipes; those
        // the hardest first, then by the rank they are trivial at and by the item's
        // level, both from the top, and by name.
        std::stable_sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
            if (a.visible != b.visible) return a.visible;
            if (!a.visible) return false;
            if (a.group != b.group) return a.group < b.group;
            if (a.header() != b.header()) return a.header();
            if (a.recipe.difficulty != b.recipe.difficulty) return a.recipe.difficulty < b.recipe.difficulty;
            if (a.recipe.trivialAt != b.recipe.trivialAt) return a.recipe.trivialAt > b.recipe.trivialAt;
            if (a.recipe.itemLevel != b.recipe.itemLevel) return a.recipe.itemLevel > b.recipe.itemLevel;
            return compareCaseless(a.recipe.name, b.recipe.name) < 0;
        });
        visible_ = 0;
        while (visible_ < rows_.size() && rows_[visible_].visible) ++visible_;
    }

    std::vector<Row> rows_;
    std::vector<Group> groups_;
    size_t visible_ = 0;
    uint32_t shownMask_ = ~0u;   ///< 0x00ad0e00
    uint32_t openMask_ = ~0u;    ///< 0x00ad0e0c
    int lastGroup_ = -1;         ///< 0x00ad0e04
};

/// A profession's link (0x005db060): its spell, the rank and the most of it, whose it is
/// and which of the line's recipes they know, a bit each, six to a letter.
struct Link {
    uint32_t spellId = 0;
    int rank = 0, maxRank = 0;
    uint64_t guid = 0;
    std::string known;
};
inline constexpr std::string_view kLinkLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// 0x005de300: a link's data - what follows its "trade:".
[[nodiscard]] inline bool readLink(const std::string& data, Link& out) {
    unsigned spell = 0;
    int rank = 0, most = 0;
    unsigned long long guid = 0;
    if (std::sscanf(data.c_str(), "%u", &spell) != 1 || spell == 0) return false;
    const size_t first = data.find(':');
    if (first == std::string::npos) return false;
    if (std::sscanf(data.c_str() + first + 1, "%d:%d:%llX:", &rank, &most, &guid) != 3 || guid == 0) return false;
    out = {spell, rank, most, guid, data.substr(data.rfind(':') + 1)};
    return true;
}
/// Whether the letters are as many as `recipes` take (0x005de120).
[[nodiscard]] constexpr bool linkFits(std::string_view known, size_t recipes) { return known.size() == (recipes + 5) / 6; }
[[nodiscard]] constexpr bool linkKnows(std::string_view known, size_t recipe) {
    const size_t letter = recipe / 6;
    const size_t value = letter < known.size() ? kLinkLetters.find(known[letter]) : std::string_view::npos;
    return value != std::string_view::npos && ((value >> (recipe % 6)) & 1) != 0;
}

}  // namespace wowee::game::trade_skill
