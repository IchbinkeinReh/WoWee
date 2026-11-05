// The direction the shadows fall from near the horizon (shadow_sun.hpp).
#include <catch_amalgamated.hpp>
#include "rendering/day_night.hpp"
#include "rendering/shadow_sun.hpp"

#include <algorithm>
#include <cmath>

using namespace wowee::rendering;

namespace {
glm::vec3 sunAt(float elevationDeg) {
    const float e = glm::radians(elevationDeg);
    return glm::vec3(std::cos(e) * 0.7071f, std::cos(e) * 0.7071f, std::sin(e));
}
float angleDeg(const glm::vec3& a, const glm::vec3& b) {
    return glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)));
}
// The light's direction in the Sunstrider evening: some twenty-seven degrees up.
const glm::vec3 kLight(0.629f, 0.629f, 0.456f);
}  // namespace

TEST_CASE("well up, the drawn sun; well down, the light", "[shadow_sun]") {
    CHECK(angleDeg(shadow_sun::towardSun(kLight, sunAt(40.0f), 0.15f), sunAt(40.0f)) < 0.01f);
    CHECK(angleDeg(shadow_sun::towardSun(kLight, sunAt(-20.0f), 0.15f), kLight) < 0.01f);
}

TEST_CASE("the drawn sun is held at the floor height", "[shadow_sun]") {
    const glm::vec3 d = shadow_sun::towardSun(kLight, sunAt(8.0f), 0.15f);  // above the band, under the floor
    CHECK(d.z == Catch::Approx(0.15f).margin(1e-4));
    CHECK(d.x == Catch::Approx(d.y).margin(1e-4));
}

TEST_CASE("crossing the horizon the shadows turn smoothly, not at once", "[shadow_sun]") {
    // A quarter of a degree of sun a step - about a minute of game time.
    float worst = 0.0f;
    glm::vec3 prev = shadow_sun::towardSun(kLight, sunAt(-15.0f), 0.15f);
    for (float e = -15.0f + 0.25f; e <= 15.0f; e += 0.25f) {
        const glm::vec3 cur = shadow_sun::towardSun(kLight, sunAt(e), 0.15f);
        worst = std::max(worst, angleDeg(prev, cur));
        prev = cur;
    }
    // Some eighteen degrees between the two, handed over across fourteen of sun.
    CHECK(worst < 0.8f);
}

TEST_CASE("the sunlight's way turns smoothly through a whole day of the real curves", "[shadow_sun]") {
    // Two minutes of game time a step. Passed unreversed, the light's direction
    // turned it 33 degrees in one step at sunrise and went up through the sky.
    float worst = 0.0f;
    float highest = -1.0f;
    glm::vec3 prev(0.0f);
    for (int minute = 0; minute <= 1440; minute += 2) {
        const float t = static_cast<float>(minute % 1440) / 1440.0f;
        const glm::vec3 cur = shadow_sun::sunTravel(daynight::directionalLightDir(t), daynight::sunDirection(t));
        if (minute > 0) worst = std::max(worst, angleDeg(prev, cur));
        highest = std::max(highest, cur.z);
        prev = cur;
    }
    CHECK(worst < 1.5f);
    CHECK(highest < 0.0f);  // sunlight comes down, day and night
}

TEST_CASE("no drawn sun, the light alone", "[shadow_sun]") {
    CHECK(angleDeg(shadow_sun::towardSun(kLight, glm::vec3(0.0f), 0.15f), kLight) < 0.01f);
}
