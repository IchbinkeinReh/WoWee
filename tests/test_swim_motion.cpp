// How a swimmer and a flyer move by their keys (rendering/swim_motion.hpp,
// 0x00987b50 and 0x00987570).
#include <catch_amalgamated.hpp>

#include "rendering/swim_motion.hpp"

using namespace wowee::rendering::collision;

namespace {

constexpr glm::vec2 kAhead{1.0f, 0.0f};
constexpr glm::vec2 kLeft{0.0f, 1.0f};

glm::vec3 go(const SwimFlyKeys& k, float pitch = 0.0f, float speed = 4.722222f) {
    return swimFlyVelocity(k, kAhead, kLeft, pitch, speed);
}

void checkVec(const glm::vec3& v, float x, float y, float z) {
    CHECK(v.x == Catch::Approx(x).margin(1e-5));
    CHECK(v.y == Catch::Approx(y).margin(1e-5));
    CHECK(v.z == Catch::Approx(z).margin(1e-5));
}

}  // namespace

TEST_CASE("a swimmer holds its depth: no key, no motion", "[swim_motion]") {
    checkVec(go({}), 0.0f, 0.0f, 0.0f);
    checkVec(go({}, 0.6f), 0.0f, 0.0f, 0.0f);
}

TEST_CASE("up and down alone go at the swim speed itself", "[swim_motion]") {
    checkVec(go({.rising = true}), 0.0f, 0.0f, 4.722222f);
    checkVec(go({.sinking = true}), 0.0f, 0.0f, -4.722222f);
    checkVec(go({.rising = true, .sinking = true}), 0.0f, 0.0f, 0.0f);
}

TEST_CASE("rising while it moves is half of each, level whatever the pitch (0x00987700)", "[swim_motion]") {
    const float h = 4.722222f * 0.70710677f;
    checkVec(go({.forward = true, .rising = true}, 0.5f), h, 0.0f, h);
    checkVec(go({.left = true, .sinking = true}, 0.5f), 0.0f, h, -h);
}

TEST_CASE("ahead goes along the pitch, aside level", "[swim_motion]") {
    const float s = 4.722222f;
    checkVec(go({.forward = true}, 0.5f), s * std::cos(0.5f), 0.0f, s * std::sin(0.5f));
    checkVec(go({.backward = true}, 0.5f), -s * std::cos(0.5f), 0.0f, -s * std::sin(0.5f));
    checkVec(go({.left = true}, 0.5f), 0.0f, s, 0.0f);
    const float d = s * 0.70710677f;
    checkVec(go({.forward = true, .right = true}, 0.5f), d * std::cos(0.5f), -d, d * std::sin(0.5f));
    checkVec(go({.forward = true, .backward = true, .left = true}), 0.0f, s, 0.0f);
}

TEST_CASE("going back is at its own speed, where that is no faster (0x00987570)", "[swim_motion]") {
    CHECK(swimFlySpeed({.forward = true}, 4.72f, 2.5f) == 4.72f);
    CHECK(swimFlySpeed({.backward = true}, 4.72f, 2.5f) == 2.5f);
    CHECK(swimFlySpeed({.backward = true, .left = true}, 4.72f, 2.5f) == 2.5f);
    CHECK(swimFlySpeed({.backward = true}, 4.72f, 9.0f) == 4.72f);
}
