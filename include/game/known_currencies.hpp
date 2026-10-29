#pragma once

#include <cstdint>

// Which currencies the token list draws.
//
// PLAYER_FIELD_KNOWN_CURRENCIES is one field read as two halves. The client
// takes the blob dword at +0x790 for currency ids 1..0x20 and the one at +0x794
// for the ids after that (0x005b0030, which is the routine that fills the token
// list), and draws a row for every currency whose bit is set there. The amount
// beside the row is read off that row's item, and is left at zero when the
// client has nothing for the item - so a currency once earned keeps its row
// after it has been spent down, which counting what is held does not say by
// itself.
//
// Currency ids start at 1, which is why both shifts are one-based. The client's
// own test is only bounded at the bottom: an id past 0x40 shifts the high half
// past its own width. This asks for an id inside the pair instead of repeating
// that, and answers not-known for anything outside it.

namespace wowee::game {

/// Which field of a CurrencyTypes.dbc row holds what.
///
/// The client reads its four-field record view (stride 0x10) at three offsets, and
/// every one of them says the same thing about which field is the id: 0x005afe30
/// asks a category whether any of the table's rows carry a name in it, 0x005b0030
/// fills the visible list, and 0x005b04f0/0x005b0590 set and clear the watched
/// pair - each of those computes the bit as 1 << (v - 1) or 1 << (v - 0x21) from
/// the field at +0xc. The item the amount is counted from is +4 (0x005b0a90
/// compares it against 43307 and 43308 (0xa92b, 0xa92c), and 0x005affd0 finds a table row by
/// matching it against the row's item), and the category the row is filed under is
/// +8 (0x005afe30 compares it against the category store's first field).
///
/// The first field is read by none of them, and it is not the id: in the table this
/// build loads it runs to 341, which is past the reach of the two dwords every one
/// of these masks is made of, while the field at +0xc runs 1..29. Keying the masks
/// on it asks for bits that cannot exist and the tab draws almost nothing.
inline constexpr uint32_t kCurrencyTypeFieldItem     = 1;   ///< +0x04
inline constexpr uint32_t kCurrencyTypeFieldCategory = 2;   ///< +0x08
inline constexpr uint32_t kCurrencyTypeFieldId       = 3;   ///< +0x0c
inline constexpr uint32_t kCurrencyTypeFields        = 4;   ///< the record's width

/// Which half, and which bit of it, a currency id addresses.
///
/// `half` is 0 for the ids 1..0x20 and 1 for the ids after that; `valid` is false
/// for an id outside the pair, which is the case the client leaves unbounded.
struct CurrencyBit {
    bool     valid = false;
    uint32_t half  = 0;
    uint32_t mask  = 0;
};

inline constexpr CurrencyBit currencyBitOf(uint32_t currencyId) {
    if (currencyId >= 1 && currencyId <= 32)  return {.valid = true, .half = 0, .mask = 1u << (currencyId - 1)};
    if (currencyId >= 33 && currencyId <= 64) return {.valid = true, .half = 1, .mask = 1u << (currencyId - 33)};
    return {};
}

/// Whether a currency's bit is set in the known-currency pair.
inline constexpr bool currencyIsKnown(uint32_t knownLow, uint32_t knownHigh,
                                      uint32_t currencyId) {
    const CurrencyBit bit = currencyBitOf(currencyId);
    if (!bit.valid) return false;
    return ((bit.half ? knownHigh : knownLow) & bit.mask) != 0;
}

/// A pair of masks over currency ids, which is how the client keeps more than one
/// set of currencies.
///
/// The watched set - the currencies shown beside the bags - is kept exactly like
/// the known pair, keyed by the same ids into the same two halves: 0x005b0590
/// computes the bit the way `currencyBitOf` does and sets or clears it, and hands
/// the result to the client's settings store, which is why the choice outlives a
/// session there. A mask kept here lives for the session only: this client has no
/// settings store for it to go through.
struct CurrencyWatch {
    uint32_t low  = 0;   ///< ids 1..0x20
    uint32_t high = 0;   ///< ids 0x21..0x40

    [[nodiscard]] bool has(uint32_t currencyId) const {
        const CurrencyBit bit = currencyBitOf(currencyId);
        return bit.valid && ((bit.half ? high : low) & bit.mask) != 0;
    }

    /// Setting an id outside the pair does nothing: there is no bit for it.
    ///
    /// There is no count here on purpose. The client sets and clears one bit
    /// however many are already set (0x005b0590 asks nothing of the rest of the
    /// pair), and the limit on how many are drawn is the interface's own:
    /// blizzard_tokenui.lua counts what GetBackpackCurrencyInfo answers and stops
    /// at MAX_WATCHED_TOKENS.
    void set(uint32_t currencyId, bool on) {
        const CurrencyBit bit = currencyBitOf(currencyId);
        if (!bit.valid) return;
        uint32_t& half = bit.half ? high : low;
        half = on ? (half | bit.mask) : (half & ~bit.mask);
    }
};

} // namespace wowee::game
