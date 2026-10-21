#pragma once

/// Whether the player meets each of an item's requirements, as the client
/// decides it for the item tooltip (0x006277f0, which draws each such line in
/// red where it is not met) and for "can this be used at all" (0x006dc3f0,
/// what GetMerchantItemInfo and GetBuybackItemInfo answer as isUsable and
/// the merchant frame tints the icon red by).
///
/// Pure: the caller gathers what the item asks and what the player has;
/// tests/test_item_requirements.cpp checks it.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

/// What an item asks, from its item cache record (SMSG_ITEM_QUERY_SINGLE_RESPONSE).
struct ItemRequirementFacts {
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
    uint32_t inventoryType = 0;
    uint32_t flags = 0;              ///< item flags (0x40000 prospectable, 0x20000000 millable)
    uint32_t allowableClass = 0;     ///< class mask (bit class-1)
    uint32_t allowableRace = 0;      ///< race mask (bit race-1)
    uint32_t requiredLevel = 0;
    uint32_t requiredSkill = 0;      ///< SkillLine id
    uint32_t requiredSkillRank = 0;
    uint32_t requiredSpell = 0;
    uint32_t requiredReputationFaction = 0;
    uint32_t requiredReputationRank = 0;  ///< 0 hated .. 7 exalted
};

/// What the player has that those requirements are measured against.
struct ItemRequirementPlayer {
    uint32_t level = 0;
    uint8_t race = 0;
    uint8_t playerClass = 0;
    /// SMSG_SET_PROFICIENCY's subclass mask for the item's class; 0 when the
    /// server sent none for it, and then no subclass is refused (0x006cde90).
    uint32_t proficiencyMask = 0;
    /// Knows a spell with the DUAL_WIELD effect (40): 0x006ceb00.
    bool canDualWield = false;
    /// The value of the item's required skill, or -1 when the player does not
    /// have the skill at all (0x006dc1c0 below zero).
    int32_t requiredSkillValue = -1;
    bool knowsRequiredSpell = false;
    /// The player's reputation with the required faction (raw value).
    int32_t requiredFactionValue = 0;
};

inline constexpr uint32_t kItemFlagProspectable = 0x00040000u;
inline constexpr uint32_t kItemFlagMillable = 0x20000000u;
inline constexpr uint32_t kInvTypeWeaponOffHand = 22;

/// The lowest raw reputation of each standing an item's rank asks for
/// (0x00a2d2fc), hated .. exalted.
inline constexpr int32_t kItemRepRankFloor[8] = {-42000, -6000, -3000, 0, 3000, 9000, 21000, 42000};

/// Which requirements are met. Each is true where the item asks nothing of it.
struct ItemRequirementVerdict {
    bool subclassOk = true;  ///< the armour or weapon type; its name goes red
    bool offHandOk = true;   ///< "Off Hand" goes red without dual wield
    bool levelOk = true;
    bool classOk = true;
    bool raceOk = true;
    bool skillOk = true;
    bool spellOk = true;
    bool reputationOk = true;

    /// 0x006dc3f0: all of them. The off-hand slot is not one of its tests.
    [[nodiscard]] bool usable() const {
        return subclassOk && levelOk && classOk && raceOk && skillOk && spellOk && reputationOk;
    }
};

/// A mask allows a unit's class or race when its bit is set. A mask of zero
/// is taken as "no restriction": the server sends -1 for that, and an item
/// this client knows only in part must not read as forbidden.
inline bool itemMaskAllows(uint32_t mask, uint32_t id) {
    if (mask == 0) return true;
    return id >= 1 && id <= 32 && (mask & (1u << (id - 1))) != 0;
}

inline ItemRequirementVerdict judgeItemRequirements(const ItemRequirementFacts& item,
                                                    const ItemRequirementPlayer& player) {
    ItemRequirementVerdict v;
    v.subclassOk = player.proficiencyMask == 0 || item.subClass >= 32 ||
                   (player.proficiencyMask & (1u << item.subClass)) != 0;
    v.offHandOk = item.inventoryType != kInvTypeWeaponOffHand || player.canDualWield;
    v.levelOk = item.requiredLevel <= player.level;
    v.classOk = itemMaskAllows(item.allowableClass, player.playerClass);
    v.raceOk = itemMaskAllows(item.allowableRace, player.race);
    if (item.requiredSkill != 0 && (item.flags & (kItemFlagProspectable | kItemFlagMillable)) == 0) {
        v.skillOk = player.requiredSkillValue >= 0 &&
                    player.requiredSkillValue >= static_cast<int32_t>(item.requiredSkillRank);
    }
    if (item.requiredSpell != 0) v.spellOk = player.knowsRequiredSpell;
    if (item.requiredReputationFaction != 0) {
        const uint32_t rank = item.requiredReputationRank < 8 ? item.requiredReputationRank : 7;
        v.reputationOk = player.requiredFactionValue >= kItemRepRankFloor[rank];
    }
    return v;
}

/// One row of ChrRaces or ChrClasses as the restriction line needs it.
struct ItemRestrictionName {
    uint32_t id = 0;
    std::string name;
    bool playable = true;  ///< ChrRaces flag 0x1 clear; every class counts
};

/// The tooltip's race and class line(s) (0x006277f0): with at most one race
/// and one class in the masks, one "%s Only" line naming them (RACE_CLASS_ONLY);
/// otherwise a "Races: ..." line where not every playable race is allowed
/// and a "Classes: ..." line where not every class is, each red when the
/// player's own is not among them.
struct ItemRestrictionLines {
    bool hasOnly = false;
    std::string onlyNames;  ///< the race's name then the class's, no separator
    bool onlyOk = true;
    std::vector<std::string> races;  ///< empty: no races line
    bool racesOk = true;
    std::vector<std::string> classes;  ///< empty: no classes line
    bool classesOk = true;
};

inline bool singleBitOrNone(uint32_t m) { return (m & (m - 1u)) == 0; }

inline ItemRestrictionLines itemRestrictionLines(uint32_t allowableClass, uint32_t allowableRace,
                                                 uint8_t playerRace, uint8_t playerClass,
                                                 const std::vector<ItemRestrictionName>& races,
                                                 const std::vector<ItemRestrictionName>& classes) {
    ItemRestrictionLines out;
    auto bit = [](uint32_t mask, uint32_t id) { return id >= 1 && id <= 32 && (mask & (1u << (id - 1))) != 0; };
    if (singleBitOrNone(allowableClass) && singleBitOrNone(allowableRace)) {
        bool ok = true;
        for (const auto& r : races) {
            if (!r.playable || r.name.empty() || !bit(allowableRace, r.id)) continue;
            out.onlyNames += r.name;
            if (playerRace != r.id) ok = false;
            break;
        }
        for (const auto& c : classes) {
            if (c.name.empty() || !bit(allowableClass, c.id)) continue;
            out.onlyNames += c.name;
            if (playerClass != c.id) ok = false;
            break;
        }
        out.hasOnly = !out.onlyNames.empty();
        out.onlyOk = ok;
        return out;
    }
    bool allRaces = true;
    for (const auto& r : races) {
        if (r.playable && !bit(allowableRace, r.id)) { allRaces = false; break; }
    }
    bool allClasses = true;
    for (const auto& c : classes) {
        if (!bit(allowableClass, c.id)) { allClasses = false; break; }
    }
    if (!allRaces) {
        out.racesOk = false;
        for (const auto& r : races) {
            if (!r.playable || r.name.empty() || !bit(allowableRace, r.id)) continue;
            out.races.push_back(r.name);
            if (playerRace == r.id) out.racesOk = true;
        }
    }
    if (!allClasses) {
        out.classesOk = false;
        for (const auto& c : classes) {
            if (c.name.empty() || !bit(allowableClass, c.id)) continue;
            out.classes.push_back(c.name);
            if (playerClass == c.id) out.classesOk = true;
        }
    }
    return out;
}

}  // namespace wowee::game
