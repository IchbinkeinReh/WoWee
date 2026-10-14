// A kit's CharProc 9 on a dynamic object: the models it drops over the
// object's area (0x007fbd70, 0x007fc220).
#include <catch_amalgamated.hpp>

#include "rendering/blizzard_object.hpp"

using namespace wowee::rendering::blizzard_object;

TEST_CASE("CharParamZero names a SpellVisualKitAreaModel row from 0", "[blizzard_object]") {
    // 0x007fbd70: rounded, its low byte, counted from the first row.
    CHECK(areaModelRow(0.0f, 15) == 0u);
    CHECK(areaModelRow(1.0f, 15) == 1u);
    CHECK(areaModelRow(14.0f, 15) == 14u);
    CHECK(areaModelRow(1.6f, 15) == 2u);
    // Past the end - -1 is 255 - the first row; no rows, none.
    CHECK(areaModelRow(15.0f, 15) == 0u);
    CHECK(areaModelRow(-1.0f, 15) == 0u);
    CHECK_FALSE(areaModelRow(0.0f, 0).has_value());
}

TEST_CASE("spellEffectLevel scales how many fall", "[blizzard_object]") {
    // 0x007fb5b0: level / 9 * 0.9 + 0.1 - all at 9, a tenth at 0.
    CHECK(rate(5.0f, 9) == Catch::Approx(5.0f));
    CHECK(rate(5.0f, 0) == Catch::Approx(0.5f));
    CHECK(rate(10.0f, 4) == Catch::Approx(5.0f));
}

TEST_CASE("a frame adds its share, at most a fifteenth of a second's", "[blizzard_object]") {
    float carried = 0.0f;
    // 5 a second over 0.1 s counts only 1/15 s: a third of one.
    CHECK(advance(carried, 0.1f, 5.0f) == 0u);
    CHECK(carried == Catch::Approx(5.0f / 15.0f));
    // Each whole one past 1 falls: 1.9 is one, and 0.9 carries.
    carried = 0.9f;
    CHECK(advance(carried, 0.05f, 20.0f) == 1u);
    CHECK(carried == Catch::Approx(0.9f));
    // Exactly 1 is not past it.
    carried = 0.0f;
    CHECK(advance(carried, 0.06f, 50.0f) == 2u);
    CHECK(carried == Catch::Approx(1.0f));
}

TEST_CASE("one falls within the radius, in the direction drawn", "[blizzard_object]") {
    const glm::vec2 centre(100.0f, 200.0f);
    const glm::vec2 east = fallPoint(centre, 8.0f, 0.5f, 0.0f);
    CHECK(east.x == Catch::Approx(104.0f));
    CHECK(east.y == Catch::Approx(200.0f));
    const glm::vec2 north = fallPoint(centre, 8.0f, 1.0f, 0.25f);
    CHECK(north.x == Catch::Approx(100.0f).margin(1e-4));
    CHECK(north.y == Catch::Approx(208.0f));
    CHECK(fallPoint(centre, 8.0f, 0.0f, 0.6f) == centre);
}

TEST_CASE("one waits a random byte, twice, in ms", "[blizzard_object]") {
    CHECK(startDelaySeconds(0.0f) == Catch::Approx(0.0f));
    CHECK(startDelaySeconds(1.0f) == Catch::Approx(0.51f));
    CHECK(startDelaySeconds(0.5f) == Catch::Approx(0.256f));
}
