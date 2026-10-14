#include <catch_amalgamated.hpp>

#include "game/rune_state.hpp"

// The death knight's runes as the client keeps them (0x00c24304..) and
// SpellRuneCost held to them (0x005ee5b0, 0x005ee7e0).

using namespace wowee::game::runes;

namespace {
const std::array<float, 4> kTenSeconds{0.1f, 0.1f, 0.1f, 0.1f};  // PLAYER_RUNE_REGEN
}

TEST_CASE("runes start as two of each, all ready (0x005edef0)", "[runes]") {
    const Runes r;
    REQUIRE(r.count == 6);
    REQUIRE(r.readyOf(0) == 2);
    REQUIRE(r.readyOf(1) == 2);
    REQUIRE(r.readyOf(2) == 2);
    REQUIRE(r.readyOf(kDeath) == 0);
}

TEST_CASE("a spell's rune list puts the runes it used on cooldown, ready ten seconds on", "[runes]") {
    Runes r;
    std::array<PowerUpdate, kMax> out{};
    const uint8_t bytes[kMax] = {};
    // Icy Touch: one frost rune (slot 4) used at 1000.
    REQUIRE(r.changed(0x3F, 0x2F, bytes, 1000, kTenSeconds, out) == 1);
    REQUIRE(out[0].rune == 4);
    REQUIRE_FALSE(out[0].usable);
    REQUIRE(r.start[4] == 1000);
    REQUIRE(r.readyAt[4] == 11000);
    REQUIRE(r.readyOf(2) == 1);
    // The update makes it ready when its time comes (0x00728a20).
    REQUIRE((r.readyBy(10999) & 0x3Fu) == 0x2Fu);  // 6 and 7, never used, read ready
    REQUIRE((r.readyBy(11000) & 0x3Fu) == 0x3Fu);
    REQUIRE(r.changed(0x2F, 0x3F, nullptr, 11000, kTenSeconds, out) == 1);
    REQUIRE(out[0].usable);
    REQUIRE(r.start[4] == 0);
}

TEST_CASE("SMSG_RESYNC_RUNES sets types and how far along each is (0x005ee440)", "[runes]") {
    Runes r;
    r.resync(0, kDeath, 255, 5000, kTenSeconds);
    REQUIRE(r.type[0] == kDeath);
    REQUIRE(r.base[0] == kDeath);
    REQUIRE(r.start[0] == -5000);
    REQUIRE(r.readyAt[0] == 5000);
    r.resync(1, 0, 0, 5000, kTenSeconds);  // just spent
    REQUIRE(r.start[1] == 5000);
    REQUIRE(r.readyAt[1] == 15000);
}

TEST_CASE("a rune cost is paid from its types, death runes standing in (0x005ee5b0)", "[runes]") {
    Runes r;
    const uint32_t frost[3] = {0, 0, 1};
    const uint32_t frostUnholy[3] = {0, 1, 1};
    REQUIRE(costMet(r, frost, 100));
    r.ready = 0x0F;  // both frost runes cooling
    REQUIRE_FALSE(costMet(r, frost, 100));
    r.type[0] = kDeath;  // a blood rune turned death pays it
    REQUIRE(costMet(r, frost, 100));
    REQUIRE_FALSE(costMet(r, frostUnholy, 200));  // doubled: two of each
}

TEST_CASE("an unpaid rune cost is ready when its runes are (0x005ee7e0)", "[runes]") {
    Runes r;
    const uint32_t frost[3] = {0, 0, 1};
    r.ready = 0x0F;
    r.readyAt[4] = 9000;
    r.readyAt[5] = 7000;
    // One frost lacking: the sooner of the pair.
    REQUIRE(readyFor(r, frost, 100) == 7000);
    const uint32_t twoFrost[3] = {0, 0, 2};
    REQUIRE(readyFor(r, twoFrost, 100) == 9000);
    r.ready = 0x3F;
    REQUIRE(readyFor(r, frost, 100) == 0);
}
