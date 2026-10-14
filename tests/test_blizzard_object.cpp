// A kit's CharProc 9 on a dynamic object: the models it drops over the
// object's area (0x007fbd70, 0x007fc220).
#include <catch_amalgamated.hpp>

#include <vector>

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

namespace {
struct Ray {
    glm::vec3 from, to;
};
}  // namespace

TEST_CASE("one lands on the first floor below its point, under no ceiling", "[blizzard_object]") {
    // 0x007fc220: a slanted way up from 1/6 above the object to 16.67 above
    // it at the point, then straight down 33.33 from there.
    const glm::vec3 centre(10.0f, 20.0f, 5.0f);
    const glm::vec2 point(14.0f, 23.0f);
    std::vector<Ray> rays;
    const auto landed = landing(centre, point, [&](const glm::vec3& from, const glm::vec3& to) -> std::optional<float> {
        rays.push_back({from, to});
        if (rays.size() == 1) return std::nullopt;  // no ceiling
        return 0.5f;                                // a floor halfway down
    });
    REQUIRE(rays.size() == 2);
    CHECK(rays[0].from == glm::vec3(10.0f, 20.0f, 5.0f + kCeilingFrom));
    CHECK(rays[0].to == glm::vec3(14.0f, 23.0f, 5.0f + kRise));
    CHECK(rays[1].from == rays[0].to);
    CHECK(rays[1].to == glm::vec3(14.0f, 23.0f, 5.0f + kRise - kDrop));
    REQUIRE(landed);
    CHECK(landed->x == Catch::Approx(14.0f));
    CHECK(landed->y == Catch::Approx(23.0f));
    CHECK(landed->z == Catch::Approx(5.0f + kRise - kDrop * 0.5f));
}

TEST_CASE("under a ceiling one is sought from 0.95 of the way to it", "[blizzard_object]") {
    const glm::vec3 centre(0.0f, 0.0f, 0.0f);
    const glm::vec2 point(8.0f, 0.0f);
    std::vector<Ray> rays;
    const auto landed = landing(centre, point, [&](const glm::vec3& from, const glm::vec3& to) -> std::optional<float> {
        rays.push_back({from, to});
        if (rays.size() == 1) return 0.5f;  // a ceiling halfway up the slant
        return 0.25f;
    });
    REQUIRE(rays.size() == 2);
    // The slant kept to 0.475 of its length: in x as well as in height.
    const float keep = 0.5f * kCeilingKeep;
    const glm::vec3 top(8.0f * keep, 0.0f, kCeilingFrom + (kRise - kCeilingFrom) * keep);
    CHECK(rays[1].from.x == Catch::Approx(top.x));
    CHECK(rays[1].from.y == Catch::Approx(0.0f));
    CHECK(rays[1].from.z == Catch::Approx(top.z));
    CHECK(rays[1].to.z == Catch::Approx(top.z - kDrop));
    REQUIRE(landed);
    CHECK(landed->x == Catch::Approx(top.x));
    CHECK(landed->z == Catch::Approx(top.z - kDrop * 0.25f));
}

TEST_CASE("with nothing below it one takes the object's place", "[blizzard_object]") {
    const auto landed = landing(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec2(4.0f, 5.0f),
                                [](const glm::vec3&, const glm::vec3&) -> std::optional<float> { return std::nullopt; });
    CHECK_FALSE(landed.has_value());
}

TEST_CASE("on a transport one is sought in the world and kept in its frame", "[blizzard_object]") {
    // 0x007fc220 with +0x184: the place put in the world by the frame, the
    // landing brought back into it; 0x007fb2d0 draws it through the frame.
    glm::mat4 frame(0.0f);  // a quarter turn about z, then moved
    frame[0] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    frame[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    frame[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    frame[3] = glm::vec4(100.0f, 200.0f, 10.0f, 1.0f);
    const glm::vec3 centre(5.0f, 0.0f, 2.0f);  // in the transport's frame
    std::vector<Ray> rays;
    const glm::vec3 place = drop(centre, &frame, 4.0f, 0.5f, 0.0f,
                                 [&](const glm::vec3& from, const glm::vec3& to) -> std::optional<float> {
                                     rays.push_back({from, to});
                                     if (rays.size() == 1) return std::nullopt;
                                     return 0.5f;
                                 });
    REQUIRE(rays.size() == 2);
    // The way up starts above the object's place in the world.
    CHECK(rays[0].from.x == Catch::Approx(100.0f));
    CHECK(rays[0].from.y == Catch::Approx(205.0f));
    CHECK(rays[0].from.z == Catch::Approx(12.0f + kCeilingFrom));
    // The point two yards east of it in the world.
    CHECK(rays[0].to.x == Catch::Approx(102.0f));
    CHECK(rays[0].to.y == Catch::Approx(205.0f));
    // Kept in the frame: two yards east in the world is two along -y there.
    CHECK(place.x == Catch::Approx(5.0f));
    CHECK(place.y == Catch::Approx(-2.0f));
    CHECK(place.z == Catch::Approx(2.0f + kRise - kDrop * 0.5f));
    const glm::vec3 drawn = drawnAt(place, &frame);
    CHECK(drawn.x == Catch::Approx(102.0f));
    CHECK(drawn.y == Catch::Approx(205.0f));
    // Landing on nothing, the object's own place, in the frame.
    CHECK(drop(centre, &frame, 4.0f, 0.5f, 0.0f, [](const glm::vec3&, const glm::vec3&) -> std::optional<float> {
              return std::nullopt;
          }) == centre);
    // With no frame the place is the world's.
    CHECK(drawnAt(place, nullptr) == place);
}
