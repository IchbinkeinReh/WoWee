// Whose face a portrait texture shows (SetPortraitTexture, 0x006113e0), and
// the views the faces are drawn in.
#include <catch_amalgamated.hpp>

#include "ui/portrait_claims.hpp"

using namespace wowee::ui::portrait_claims;

TEST_CASE("a claim names a unit", "[portrait]") {
    const uint64_t banker = 0xF13000232D000C7Aull;
    CHECK(unitOf(unitKey(banker)) == banker);
    CHECK(unitOf(unitKey(1)) == 1u);
    CHECK_FALSE(unitOf(kPlayer).has_value());
    CHECK_FALSE(unitOf("npc").has_value());
    CHECK(unitKey(banker) != unitKey(banker + 1));
}

TEST_CASE("each claimed unit has a view of its own", "[portrait]") {
    Pool pool;
    const std::string banker = unitKey(1), target = unitKey(2), focus = unitKey(3);
    pool.assign({banker, target});
    REQUIRE(pool.viewOf(banker));
    REQUIRE(pool.viewOf(target));
    CHECK(pool.viewOf(banker) != pool.viewOf(target));
    const auto bankers = *pool.viewOf(banker);
    pool.assign({target, focus, banker});
    CHECK(pool.viewOf(banker) == bankers);
    CHECK(pool.views() == 3);
}

TEST_CASE("a face nobody claims keeps its view until another needs it", "[portrait]") {
    Pool pool;
    pool.assign({unitKey(1)});
    const auto first = *pool.viewOf(unitKey(1));
    pool.assign({});
    CHECK(pool.viewOf(unitKey(1)) == first);
    // Up to the ones kept, each new unit has a new view ...
    for (uint64_t g = 2; g <= Pool::kKept; ++g) pool.assign({unitKey(g)});
    CHECK(pool.views() == Pool::kKept);
    CHECK(pool.viewOf(unitKey(1)) == first);
    // ... and then the longest unclaimed one's.
    pool.assign({unitKey(100)});
    CHECK(pool.views() == Pool::kKept);
    CHECK_FALSE(pool.viewOf(unitKey(1)));
    CHECK(pool.viewOf(unitKey(100)) == first);
    CHECK(pool.viewOf(unitKey(2)));
}

TEST_CASE("the faces on screen are never taken from each other", "[portrait]") {
    Pool pool;
    std::vector<std::string> all;
    for (uint64_t g = 1; g <= Pool::kKept + 3; ++g) all.push_back(unitKey(g));
    pool.assign(all);
    CHECK(pool.views() == all.size());
    for (const auto& key : all) CHECK(pool.viewOf(key));
}
