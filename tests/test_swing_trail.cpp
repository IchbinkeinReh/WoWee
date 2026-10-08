// A weapon's swing trail (SWING, 0x007e4c30 to 0x007e4ff0), which a spell
// kit's CharProc 8 starts on the weapons in a unit's hands.
#include <catch_amalgamated.hpp>

#include "rendering/swing_trail.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace st = wowee::rendering::swing_trail;
using wowee::rendering::client_ribbon::Vertex;

TEST_CASE("CharProc 8's colour and time, chopped (0x007265c0 case 8)", "[swing_trail]") {
    const auto s = st::startFor(16744448.0f /* 0xFF8000 */, 300.7f, 200.9f);
    CHECK(s.colour == 0xC8FF8000u);
    CHECK(s.durationMs == 300);
    CHECK(st::kEventBladeBottom == 0x42545724u);  // "$WTB"
    CHECK(st::kEventBladeTop == 0x54545724u);     // "$WTT"
}

TEST_CASE("a swing trail lays the blade down and fades from its old end (0x007e4ce0)", "[swing_trail]") {
    st::Trail trail;
    trail.start(st::startFor(16744448.0f, 300.0f, 255.0f), 1000);
    std::vector<Vertex> strip;
    const glm::vec3 bottom(0.0f, 0.0f, 0.0f);
    // The first frame lays one pair: nothing to draw yet.
    REQUIRE(trail.step(1010, glm::vec3(0.0f, 0.0f, 1.0f), bottom, strip));
    CHECK(strip.empty());
    // The second, two pairs: bottom then top, a pair's alpha a step (10 ms
    // of 255 over 300 ms, chopped: 8) below the one before.
    REQUIRE(trail.step(1020, glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f), strip));
    REQUIRE(strip.size() == 4);
    CHECK(strip[0].position == bottom);
    CHECK(strip[1].position == glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK((strip[0].color >> 24) == 247);
    CHECK((strip[1].color >> 24) == 247);
    CHECK((strip[2].color >> 24) == 239);
    CHECK((strip[0].color & 0xFFFFFFu) == 0xFF8000u);
    // It lays nothing after its time and fades out, then lets go.
    uint32_t now = 1020;
    bool alive = true;
    while (alive && now < 20000) {
        now += 10;
        alive = trail.step(now, glm::vec3(0.0f, 0.0f, 1.0f), bottom, strip);
    }
    CHECK_FALSE(alive);
    CHECK(now < 3000);
}

TEST_CASE("a swing trail is untextured, alpha blended and not written (0x007e4ce0)", "[swing_trail]") {
    const auto m = st::material();
    CHECK(m.blend == 2);
    CHECK_FALSE(m.depthWrite);
    CHECK(m.depthTest);
    CHECK_FALSE(m.fogged);
    CHECK_FALSE(m.lit);
    CHECK_FALSE(m.cull);
}
