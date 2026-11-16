// A combat log's delta on a unit's predicted health (0x0071c260).
#include <catch_amalgamated.hpp>

#include "game/power_prediction.hpp"

using wowee::game::power_prediction::predictedHealthAfter;

TEST_CASE("damage and healing move the predicted health", "[predictedhealth]") {
    CHECK(predictedHealthAfter(500, -200, 1000, 0) == 300);
    CHECK(predictedHealthAfter(500, 200, 1000, 0) == 700);
}

TEST_CASE("the predicted health stays between 1 and the maximum", "[predictedhealth]") {
    CHECK(predictedHealthAfter(100, -500, 1000, 0) == 1);
    CHECK(predictedHealthAfter(100, -100, 1000, 0) == 1);
    CHECK(predictedHealthAfter(900, 500, 1000, 0) == 1000);
}

TEST_CASE("UNIT_FIELD_FLAGS_2 0x100 stops a loss but not a gain", "[predictedhealth]") {
    CHECK(predictedHealthAfter(500, -200, 1000, 0x100) == 500);
    CHECK(predictedHealthAfter(500, 200, 1000, 0x100) == 700);
}
