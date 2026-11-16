// The tracked-achievement list and the CVar it is saved in (0x005b23e0,
// 0x005b24d0, 0x00766720, 0x00766f70).
#include <catch_amalgamated.hpp>

#include "game/achievement_tracking.hpp"

namespace at = wowee::game::achievement_tracking;

TEST_CASE("tracked achievements keep the order they were added in", "[achievement]") {
    std::vector<uint32_t> list;
    CHECK(at::track(list, 4000) == at::TrackResult::Added);
    CHECK(at::track(list, 12) == at::TrackResult::Added);
    CHECK(at::track(list, 4000) == at::TrackResult::AlreadyTracked);
    CHECK(list == std::vector<uint32_t>{4000, 12});
    CHECK(at::untrack(list, 4000));
    CHECK_FALSE(at::untrack(list, 4000));
    CHECK(list == std::vector<uint32_t>{12});
}

TEST_CASE("an eleventh tracked achievement is refused", "[achievement]") {
    std::vector<uint32_t> list;
    for (uint32_t id = 1; id <= 10; ++id) CHECK(at::track(list, id) == at::TrackResult::Added);
    CHECK(at::track(list, 11) == at::TrackResult::Full);
    CHECK(at::track(list, 10) == at::TrackResult::AlreadyTracked);
    CHECK(list.size() == 10);
}

TEST_CASE("the trackedAchievements value is the client's packed list", "[achievement]") {
    // 'v' 0x01, then three base-93 characters from '#' per id.
    CHECK(at::encode({}) == std::string("v\x01"));
    const std::string one = at::encode({1});
    CHECK(one == std::string("v\x01##$"));
    // 4000 = 0*8649 + 43*93 + 1
    CHECK(at::encode({4000}) == std::string("v\x01#N$"));
    const std::vector<uint32_t> ids{1, 4000, 8649, 800000};
    CHECK(at::decode(at::encode(ids)) == ids);
    // Out of range ids are not written.
    CHECK(at::encode({0xc4605}) == std::string("v\x01"));
    // No header, nothing; a cut-off triple or a zero ends the list.
    CHECK(at::decode("##$").empty());
    CHECK(at::decode(std::string("v\x01##$#N")) == std::vector<uint32_t>{1});
    CHECK(at::decode(std::string("v\x01###") + "##$").empty());
}
