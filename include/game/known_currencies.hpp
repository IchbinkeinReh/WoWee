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

/// Whether a currency's bit is set in the known-currency pair.
inline constexpr bool currencyIsKnown(uint32_t knownLow, uint32_t knownHigh,
                                      uint32_t currencyId) {
    if (currencyId >= 1 && currencyId <= 32)
        return ((knownLow >> (currencyId - 1)) & 1u) != 0;
    if (currencyId >= 33 && currencyId <= 64)
        return ((knownHigh >> (currencyId - 33)) & 1u) != 0;
    return false;
}

} // namespace wowee::game
