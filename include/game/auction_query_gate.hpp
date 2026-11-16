#pragma once

// When the auction house lets a query go out, as the client decides it.
//
// A getAll query - every auction at once - may be sent once every fifteen
// minutes (900000 ms) from the last one this session sent (0x0059bcf0 keeps
// the time at 0x00c0f418, set only when a getAll goes out and cleared only at
// start-up); CanSendAuctionQuery's second answer (0x0059c740) and
// QueryAuctionItems's own refusal read the same test.
//
// The row an auction list has selected is kept as the auction's id, not its
// row (SetSelectedAuctionItem 0x0059c830 -> 0x0059b2e0 for "list", 0x0059b320
// "owner", 0x0059b360 "bidder"), so GetSelectedAuctionItem (0x0059c920) finds
// the row again after the list is re-sorted or re-sent, and answers 0 once the
// auction is gone from it.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::game {

constexpr uint64_t kAuctionGetAllIntervalMs = 900000;

/// Whether a getAll query may go out now. `lastMs` is when the last one went,
/// 0 for never this session.
inline bool auctionGetAllAllowed(uint64_t lastMs, uint64_t nowMs) {
    return lastMs == 0 || nowMs - lastMs >= kAuctionGetAllIntervalMs;
}

/// No auction selected.
constexpr uint32_t kNoAuctionSelected = 0xFFFFFFFFu;

/// The id to keep for selecting row `index0` (from 0) of a list whose rows
/// carry `ids`: that row's auction id, or none past the end.
inline uint32_t auctionSelectionFor(const std::vector<uint32_t>& ids, int index0) {
    if (index0 < 0 || static_cast<size_t>(index0) >= ids.size()) return kNoAuctionSelected;
    return ids[static_cast<size_t>(index0)];
}

/// The row (from 0) holding the kept id, or -1 when none does.
inline int auctionSelectionRow(const std::vector<uint32_t>& ids, uint32_t selected) {
    if (selected == kNoAuctionSelected) return -1;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == selected) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace wowee::game
