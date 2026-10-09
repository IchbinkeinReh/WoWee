// The tracking menu's own entries (game/minimap_tracking.hpp; 0x00a11c50,
// 0x0057eb00, 0x0057e070).
#include <catch_amalgamated.hpp>

#include "game/minimap_tracking.hpp"

#include <algorithm>

namespace mt = wowee::game::minimap_tracking;

TEST_CASE("fifteen entries, the mailbox and low-level quests last", "[minimap_tracking]") {
    REQUIRE(mt::kOther.size() == 15u);
    CHECK(mt::kOther[13].kind == mt::Kind::GameObjectType);
    CHECK(mt::kOther[13].mask == 19u);
    CHECK(mt::kOther[14].kind == mt::Kind::TrivialQuests);
    CHECK(mt::kOther[0].mask == 0x1000u);
}

TEST_CASE("offered by class", "[minimap_tracking]") {
    // A rogue sees poisons and ammunition, a hunter the stable master and
    // ammunition, a mage none of the three.
    const auto rogue = mt::offered(4);
    const auto hunter = mt::offered(3);
    const auto mage = mt::offered(8);
    const auto has = [](const std::vector<int>& v, int i) { return std::find(v.begin(), v.end(), i) != v.end(); };
    CHECK(has(rogue, 2));
    CHECK(has(rogue, 3));
    CHECK_FALSE(has(rogue, 7));
    CHECK(has(hunter, 7));
    CHECK(has(hunter, 3));
    CHECK_FALSE(has(hunter, 2));
    CHECK(mage.size() == 12u);
    CHECK(has(mt::offered(1), 3));
}

TEST_CASE("kept by name", "[minimap_tracking]") {
    CHECK(mt::byName("MINIMAP_TRACKING_MAILBOX") == 13);
    CHECK(mt::byName("") == -1);
    CHECK(mt::byName(nullptr) == -1);
    CHECK(mt::byName("nonsense") == -1);
}
