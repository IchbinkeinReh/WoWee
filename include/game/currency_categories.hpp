#pragma once

#include <cstdint>
#include <string>

// The groups the token tab files currencies under (CurrencyCategory.dbc).
//
// The client keeps the table in its own store (0x005afd90): three dwords per
// category - the id, an expanded flag initialised to 1, and the name - copied out
// of the table's projection, then sorted with 0x005afc40. That comparator puts the
// category carrying flag bit 0 last and compares the names of the rest with
// 0x0076ec80, which is the client's locale collation rather than a byte compare
// (0x0076eb80 walks the characters through the locale's collation table and
// 0x0076eab0 returns a weight). Being last is not cosmetic: the last category is
// the one the client files the *unused* currencies under, and 0x005afe30 asks for
// it by position (param_1 == the store's count - 1), never by id or by name.
//
// So the file's own order is irrelevant and a category with no record drops its
// currencies: 0x005b0030 walks categories and asks each in turn for its rows, so a
// currency whose category id names nothing is never reached. In the table this
// build loads one category id used by the currencies (24) has no record, and four
// records (4, 21, 23, 41) have no currencies.
//
// The name compare is left as a byte compare here. For names that differ in their
// first letters - all eight of the names in this build's table - a byte compare
// gives the same order; it parts company with a collation only on names differing
// by case or by an accented letter, which no category in this table does.

namespace wowee::game {

struct CurrencyCategory {
    uint32_t    id = 0;
    uint32_t    flags = 0;   ///< bit 0: the unused bucket, which sorts last
    std::string name;        ///< read out of the table's locale block
};

/// Which column of CurrencyCategory.dbc holds what, in the file this build reads.
///
/// Nineteen fields: the id, the flags, and a seventeen-column locale block (eight
/// string offsets, eight flag words and one tail word) running to the end of the
/// record - the same shape SkillLineCategory.dbc has, and the shape
/// pipeline/dbc_loader.cpp detects and promotes: it copies the active locale's
/// offset into the block's first column, so the name is read at field 2. Read
/// through that loader against this build's table, the promotion lands there and
/// field 1 is left holding its own value (only "Unbenutzt" has one, and it is 3).
/// The block's deDE offset sits at field 5 before promotion.
inline constexpr uint32_t kCurrencyCategoryFieldId   = 0;
inline constexpr uint32_t kCurrencyCategoryFieldFlag = 1;
inline constexpr uint32_t kCurrencyCategoryFieldName = 2;   ///< promoted
inline constexpr uint32_t kCurrencyCategoryFields    = 3;

/// Flag bit 0, which the client tests as (+0xc & 1) before it compares names.
///
/// One row of this build's table carries it: id 3, "Unbenutzt" - and its flags are
/// 3, so bit 1 is set too and means nothing to the client, which only ever looks
/// at bit 0.
inline constexpr uint32_t kCurrencyCategoryUnusedGroup = 1;

inline constexpr bool categorySortsLast(uint32_t flags) {
    return (flags & kCurrencyCategoryUnusedGroup) != 0;
}

/// The store's own order (0x005afc40): unflagged categories by name, flagged last.
inline bool currencyCategoryLess(const CurrencyCategory& a, const CurrencyCategory& b) {
    const bool aLast = categorySortsLast(a.flags);
    const bool bLast = categorySortsLast(b.flags);
    if (aLast != bLast) return !aLast;
    return a.name < b.name;
}

/// Whether a currency's row belongs under this category (0x005b0030's two legs).
///
/// An ordinary category takes the currencies filed under its id whose unused bit
/// is clear; the last one takes exactly the currencies whose unused bit is set,
/// whatever id they carry. Both legs have already required the realm's known bit,
/// which is what `knownBit` in `categoryIsDrawn` is for.
inline constexpr bool currencyBelongsHere(bool lastCategory, uint32_t currencyCategoryId,
                                          uint32_t categoryId, bool unusedBit) {
    return lastCategory ? unusedBit
                        : (currencyCategoryId == categoryId && !unusedBit);
}

/// Whether the category gets a header at all (0x005afe30).
///
/// The client walks the whole currency table per category and returns as soon as
/// one row answers: an ordinary category is drawn when a currency the realm knows
/// is filed under it, and the last one when a currency the realm knows is marked
/// unused. A category that no known currency reaches gets no header and no rows.
inline constexpr bool categoryIsDrawn(bool categoryMatches, bool knownBit,
                                      bool lastCategory, bool unusedBit) {
    return (categoryMatches && knownBit) || (lastCategory && unusedBit);
}

} // namespace wowee::game
