// The auction house's getAll interval and per-list selection
// (0x0059c740, 0x0059bcf0, 0x0059b2e0, 0x0059b3a0).
#include <catch_amalgamated.hpp>

#include "game/auction_query_gate.hpp"

using namespace wowee::game;

TEST_CASE("a getAll query waits fifteen minutes from the last one", "[auction]") {
    CHECK(auctionGetAllAllowed(0, 5));
    CHECK_FALSE(auctionGetAllAllowed(1000, 1000 + 899999));
    CHECK(auctionGetAllAllowed(1000, 1000 + 900000));
}

TEST_CASE("a selected auction is found again by its id", "[auction]") {
    std::vector<uint32_t> rows{70, 71, 72};
    const uint32_t kept = auctionSelectionFor(rows, 1);
    CHECK(kept == 71);
    CHECK(auctionSelectionRow(rows, kept) == 1);
    // Re-sorted: the same auction, another row.
    rows = {72, 71, 70};
    CHECK(auctionSelectionRow(rows, kept) == 1);
    rows = {71, 70};
    CHECK(auctionSelectionRow(rows, kept) == 0);
    // Gone from the list: no row.
    rows = {70};
    CHECK(auctionSelectionRow(rows, kept) == -1);
    // Past the end selects nothing.
    CHECK(auctionSelectionFor(rows, 5) == kNoAuctionSelected);
    CHECK(auctionSelectionRow(rows, kNoAuctionSelected) == -1);
}
