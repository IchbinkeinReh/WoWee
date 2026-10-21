// SMSG_AUCTION_LIST_PENDING_SALES's mails as the owner's list's rows
// (0x0059e880): the subject read as 0x0059a4a0 reads it, the body's buyer,
// bid and buyout, the days to come.
#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/auction_pending_sales.hpp"

namespace wowee::core {
Application* Application::instance = nullptr;
}

namespace ap = wowee::game::auction_pending;

TEST_CASE("A pending sale's subject: four numbers, the third 2", "[auction]") {
    int32_t f[4];
    CHECK(ap::subjectFields("12345:0:2:678", f) == 4);
    CHECK(f[0] == 12345);
    CHECK(f[2] == 2);
    CHECK(f[3] == 678);
    CHECK(ap::subjectFields("12345:0:6", f) == 3);
    CHECK(ap::subjectFields("12345", f) == 1);
    CHECK(ap::subjectFields("", f) == 1);
}

TEST_CASE("A pending sale becomes a row of the owner's list", "[auction]") {
    const ap::Mail mail{.subject = "33470:-12:2:9001", .body = "0000000000ABCDEF:500:900:25:45", .daysLeft = 0.5f};
    const auto row = ap::row(mail, 0x42);
    REQUIRE(row);
    CHECK(row->itemEntry == 33470);
    CHECK(row->randomPropertyId == static_cast<uint32_t>(-12));
    CHECK(row->auctionId == 9001);
    CHECK(row->stackCount == 0);
    CHECK(row->ownerGuid == 0x42);
    CHECK(row->bidderGuid == 0xABCDEFu);
    CHECK(row->currentBid == 500);
    CHECK(row->buyoutPrice == 900);
    CHECK(row->timeLeftMs == 43200000u);
    CHECK(row->pendingSale);
    // Another kind of auction mail, or one short of a field, is no row.
    CHECK_FALSE(ap::row({.subject = "33470:0:1:9001", .body = "", .daysLeft = 1.0f}, 0x42));
    CHECK_FALSE(ap::row({.subject = "33470:0:2", .body = "", .daysLeft = 1.0f}, 0x42));
}
