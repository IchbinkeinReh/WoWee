#pragma once

/// Choices the client's item tooltip builder (0x006277f0) makes from an
/// item's template alone, kept pure so tests/test_item_tooltip_rules.cpp can
/// pin them to the decompile.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::game::item_tooltip {

inline constexpr uint32_t kItemClassMoney = 10;
inline constexpr uint32_t kItemFlagUniqueEquipped = 0x80000;  ///< +0x18 bit 19

/// The uniqueness line under the binding (0x00627fb4..0x00628120): none for
/// a class 10 item; ITEM_UNIQUE_EQUIPPABLE for flag 0x80000; else by
/// MaxCount - ITEM_UNIQUE for one, ITEM_UNIQUE_MULTIPLE ("Unique (%d)") for
/// more; else by ItemLimitCategory - ITEM_LIMIT_CATEGORY_MULTIPLE when the
/// category's flag 0x1 is set ("Unique-Equipped: %s (%d)"), else
/// ITEM_LIMIT_CATEGORY ("Unique: %s (%d)").
struct UniqueLine {
    const char* key = nullptr;  ///< null: no line
    bool count = false;         ///< the key takes MaxCount
    bool category = false;      ///< the key takes the category's name and quantity
};

inline UniqueLine uniqueLine(uint32_t itemClass, uint32_t flags, int32_t maxCount, bool hasLimitCategory,
                             uint32_t limitCategoryFlags) {
    if (itemClass == kItemClassMoney) return {};
    if ((flags & kItemFlagUniqueEquipped) != 0) return {"ITEM_UNIQUE_EQUIPPABLE", false, false};
    if (maxCount >= 1) return maxCount == 1 ? UniqueLine{"ITEM_UNIQUE", false, false}
                                            : UniqueLine{"ITEM_UNIQUE_MULTIPLE", true, false};
    if (hasLimitCategory)
        return {(limitCategoryFlags & 0x1u) != 0 ? "ITEM_LIMIT_CATEGORY_MULTIPLE" : "ITEM_LIMIT_CATEGORY", false,
                true};
    return {};
}

/// The item level line (0x0062b1ad): only with showItemLevel, and only for
/// weapons (2), armour (4), reagents (5) and projectiles (6).
inline bool showsItemLevel(uint32_t itemClass) {
    return itemClass == 2 || itemClass == 4 || itemClass == 5 || itemClass == 6;
}

/// A glyph's kind (0x006286f0): GlyphProperties' TypeFlags (+8) zero is a
/// major glyph, anything else a minor one.
inline const char* glyphKey(uint32_t glyphTypeFlags) { return glyphTypeFlags == 0 ? "MAJOR_GLYPH" : "MINOR_GLYPH"; }
inline constexpr uint32_t kSpellEffectApplyGlyph = 74;

/// How many pieces a set has, as its name line counts them (0x0062ba00):
/// one for each piece whose inventory type no piece before it had - rings,
/// trinkets and one-hand weapons (11, 12, 13) excepted, which each count,
/// and a chest (5) and a robe (20) being the same slot. `invTypes` holds
/// each piece's inventory type, 0 for a piece not known (no item, or not in
/// the cache yet) - which counts nothing, and as the client's walk stops at
/// it, no piece after it counts either.
inline uint32_t setPieceTotal(const std::vector<uint32_t>& invTypes) {
    uint32_t total = 0;
    for (size_t i = 0; i < invTypes.size(); ++i) {
        const uint32_t t = invTypes[i];
        if (t == 0) continue;
        size_t j = 0;
        for (; j < i; ++j) {
            const uint32_t u = invTypes[j];
            if (u == 0) break;
            if (t == u && t != 11 && t != 12 && t != 13) break;
            if ((t == 5 && u == 20) || (t == 20 && u == 5)) break;
        }
        if (j == i) ++total;
    }
    return total;
}

/// What an item that can be prospected or milled says in place of its skill
/// line (0x0062b2a0): ITEM_PROSPECTABLE for flag 0x40000, ITEM_MILLABLE for
/// 0x20000000, null otherwise.
inline const char* processKey(uint32_t flags) {
    if ((flags & 0x40000u) != 0) return "ITEM_PROSPECTABLE";
    if ((flags & 0x20000000u) != 0) return "ITEM_MILLABLE";
    return nullptr;
}

}  // namespace wowee::game::item_tooltip
