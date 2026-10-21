// The statistics most recently updated, newest first (0x005b2520, 0x005b2cb0),
// which GetLatestUpdatedStats and GetLatestUpdatedComparisonStats read
// (0x005b1c90, 0x005b1d60).

#include <catch_amalgamated.hpp>

#include "game/latest_criteria.hpp"

#include <map>

using wowee::game::LatestCriteriaList;

namespace {

// Criteria 10x belong to achievement x; 999 has no row.
uint32_t achievementOf(uint32_t criteria) { return criteria == 999 ? 0 : criteria / 10; }

}  // namespace

TEST_CASE("The newest statistic is first and one per statistic is kept", "[latest_criteria]") {
    LatestCriteriaList list;
    list.notePlayer(11, achievementOf);
    list.notePlayer(21, achievementOf);
    list.notePlayer(12, achievementOf);  // achievement 1 again: replaces 11
    CHECK(list.all() == std::vector<uint32_t>{12, 21});
    list.notePlayer(21, achievementOf);  // the same criteria again
    CHECK(list.all() == std::vector<uint32_t>{21, 12});
}

TEST_CASE("Six are held and five are read", "[latest_criteria]") {
    LatestCriteriaList list;
    for (uint32_t a = 1; a <= 7; ++a) list.notePlayer(a * 10, achievementOf);
    CHECK(list.all() == std::vector<uint32_t>{70, 60, 50, 40, 30, 20});
    CHECK(list.latest() == std::vector<uint32_t>{70, 60, 50, 40, 30});
}

TEST_CASE("A held criteria with no row stalls the walk", "[latest_criteria]") {
    LatestCriteriaList list;
    list.notePlayer(11, achievementOf);
    list.notePlayer(999, achievementOf);
    REQUIRE(list.all() == std::vector<uint32_t>{999, 11});
    // The walk stands on 999 for all five steps and then drops it, so the
    // older record of the same statistic is never reached.
    list.notePlayer(12, achievementOf);
    CHECK(list.all() == std::vector<uint32_t>{12, 11});
}

TEST_CASE("The comparison list replaces only the same criteria", "[latest_criteria]") {
    LatestCriteriaList list;
    list.noteComparison(11);
    list.noteComparison(12);
    list.noteComparison(11);
    CHECK(list.all() == std::vector<uint32_t>{11, 12});
}
