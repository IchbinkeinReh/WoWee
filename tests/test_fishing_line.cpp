// The fishing line's shape (0x006f8f50).
#include <catch_amalgamated.hpp>

#include "rendering/fishing_line_geometry.hpp"

using namespace wowee::rendering;

TEST_CASE("the fishing line hangs from the tip to the bobber as 0x006f8f50 draws it", "[fishing]") {
    const glm::vec3 tip(1.0f, 2.0f, 10.0f);
    const glm::vec3 end(9.0f, 2.0f, 2.0f);
    const auto pts = fishing_line::points(tip, end);
    REQUIRE(pts.size() == 65);
    // The ends are the tip and the bobber themselves.
    CHECK(pts.front().x == Catch::Approx(tip.x));
    CHECK(pts.front().z == Catch::Approx(tip.z));
    CHECK(pts.back().x == Catch::Approx(end.x));
    CHECK(pts.back().z == Catch::Approx(end.z).margin(1e-5));
    // Half a yard below the straight line at its middle.
    CHECK(pts[32].x == Catch::Approx(5.0f));
    CHECK(pts[32].z == Catch::Approx(6.0f - 0.5f));
    // A quarter of the way: 0.5 sin(pi/4).
    CHECK(pts[16].z == Catch::Approx(8.0f - 0.5f * std::sin(3.1415927f / 4.0f)));
}

TEST_CASE("the line meets the bobber a fifth of its model up", "[fishing]") {
    const glm::vec3 e = fishing_line::bobberEnd(glm::vec3(3.0f, 4.0f, 5.0f), 2.0f, 0.5f);
    CHECK(e.x == Catch::Approx(3.0f));
    CHECK(e.z == Catch::Approx(5.0f + 2.0f * 0.5f * 0.2f));
}

TEST_CASE("the line's colour is the pole's ambient, held within 0..1", "[fishing]") {
    const glm::vec3 c = fishing_line::colour(glm::vec3(1.5f, -0.2f, 0.4f));
    CHECK(c.r == Catch::Approx(1.0f));
    CHECK(c.g == Catch::Approx(0.0f));
    CHECK(c.b == Catch::Approx(0.4f));
}
