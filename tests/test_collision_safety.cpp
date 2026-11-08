// The collision port's test safety (collision_safety.hpp): a runaway fall puts
// the character back where it last stood, but only in the world it stood in.

#include <catch_amalgamated.hpp>

#include "rendering/collision_safety.hpp"

using namespace wowee::rendering::collision;

TEST_CASE("a fall far below the place last stood on is put back there", "[collision_safety]") {
    SafePlace safe;
    safe.stoodAt({2078.13f, 299.02f, 98.42f});
    const auto back = safe.runawayRestore({2079.0f, 300.0f, 50.0f}, false);
    REQUIRE(back.has_value());
    CHECK(back->z == 98.42f);
    // Nothing at all below is a runaway however short the drop.
    CHECK(safe.runawayRestore({2079.0f, 300.0f, 97.0f}, true).has_value());
    // An ordinary fall is not.
    CHECK_FALSE(safe.runawayRestore({2079.0f, 300.0f, 90.0f}, false).has_value());
}

// The zeppelin from Undercity to Orgrimmar, 2026-11-08: the map change put
// the character in Durotar, and the last place stood on - the deck over
// Tirisfal, on map 0 - was taken for where she had fallen from.
TEST_CASE("a place from before a map change is never restored", "[collision_safety]") {
    SafePlace safe;
    safe.stoodAt({2078.13f, 299.02f, 98.42f});
    // Forgotten at the map change, as at a teleport or a transport boarded.
    safe.forget();
    CHECK_FALSE(safe.valid());
    CHECK_FALSE(safe.runawayRestore({1281.35f, -4817.11f, 54.42f}, true).has_value());
}

TEST_CASE("a place a world away is not this world's, even if not forgotten", "[collision_safety]") {
    SafePlace safe;
    safe.stoodAt({2078.13f, 299.02f, 98.42f});
    // Durotar, with nothing loaded below yet and far below the deck.
    CHECK_FALSE(safe.runawayRestore({1281.35f, -4817.11f, 54.42f}, true).has_value());
    CHECK_FALSE(safe.runawayRestore({2078.13f + SafePlace::kSameWorldReach + 1.0f, 299.02f, 0.0f}, true).has_value());
    // Within reach it still holds.
    CHECK(safe.runawayRestore({2078.13f + SafePlace::kSameWorldReach - 1.0f, 299.02f, 0.0f}, false).has_value());
}
