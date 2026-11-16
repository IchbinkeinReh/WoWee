// CalculateAuctionDeposit's arithmetic (0x0059d270, 0x0076dde0).
#include <catch_amalgamated.hpp>

#include "game/auction_deposit.hpp"

using namespace wowee::game::auction_deposit;

TEST_CASE("the run time picks the minutes, 1440 otherwise", "[auctiondeposit]") {
    CHECK(minutesForRunTime(1) == 720);
    CHECK(minutesForRunTime(2) == 1440);
    CHECK(minutesForRunTime(3) == 2880);
    CHECK(minutesForRunTime(0) == 1440);
    CHECK(minutesForRunTime(4) == 1440);
    CHECK(minutesForRunTime(720) == 1440);
}

TEST_CASE("the deposit is the house's percentage per 240 minutes", "[auctiondeposit]") {
    // 5% of 10000 copper is 500; twelve hours is three 240-minute blocks.
    CHECK(deposit(5, 10000, 720) == 1500);
    CHECK(deposit(5, 10000, 1440) == 3000);
    CHECK(deposit(5, 10000, 2880) == 6000);
    CHECK(deposit(25, 10000, 720) == 7500);
}

TEST_CASE("the percentage is taken in whole copper", "[auctiondeposit]") {
    // 5% of 1999 is 99.95, kept as 99: 99 x 3 = 297.
    CHECK(deposit(5, 1999, 720) == 297);
}

TEST_CASE("never under a hundred, and a hundred for an unknown length", "[auctiondeposit]") {
    CHECK(deposit(5, 100, 720) == 100);
    CHECK(deposit(5, 1000000, 600) == 100);
}
