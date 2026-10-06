// The original client's DayNight pieces that need no DBC (Wow.exe 3.3.5a,
// build 12340): where the light, the sun and the moon are at each hour, how
// much of one Light.dbc volume shows, the order the volumes go on in, the
// storm blend and the fog range.
//
// The light used to sweep round the sky through the day, lie flat at 06:00 and
// 18:00 and point upward at midnight, so every lit surface was ambient-only at
// night and shadows stretched to infinity at dawn. The volumes were two at
// most, renormalised to sum to one, with smoothstep falloff and the map's
// default light never counted.
#include <catch_amalgamated.hpp>

#include "rendering/day_night.hpp"
#include "rendering/lighting_manager.hpp"
#include "test_support.hpp"

#include <cmath>
#include <regex>
#include <string>

namespace dn = wowee::rendering::daynight;

namespace {

float elevationDeg(const glm::vec3& towardBody) {
    return glm::degrees(std::asin(glm::clamp(towardBody.z, -1.0f, 1.0f)));
}

}  // namespace

TEST_CASE("the client's cosine is the cubic it computes with", "[daynight]") {
    CHECK(dn::clientCosPi(0.0f) == Catch::Approx(1.0f));
    CHECK(dn::clientCosPi(0.5f) == Catch::Approx(0.0f).margin(1e-6));
    CHECK(dn::clientCosPi(1.0f) == Catch::Approx(-1.0f));
    CHECK(dn::clientCosPi(2.0f) == Catch::Approx(1.0f));
    CHECK(dn::clientCosPi(-1.0f) == Catch::Approx(-1.0f));
    // Close to the real thing, not equal to it: 0.6875 against 0.7071.
    CHECK(dn::clientCosPi(0.25f) == Catch::Approx(0.6875f));
    for (float u = -2.0f; u <= 2.0f; u += 0.01f) {
        CHECK(std::abs(dn::clientCosPi(u) - std::cos(u * 3.14159265f)) < 0.025f);
    }
}

TEST_CASE("a time curve interpolates and wraps across midnight", "[daynight]") {
    // Halfway between two keys.
    CHECK(dn::sampleCurve(dn::kLightPhi, 0.125f) ==
          Catch::Approx((2.2165682f + 1.9198622f) * 0.5f));
    // Between the last key and the first, through midnight.
    CHECK(dn::sampleCurve(dn::kLightPhi, 0.875f) ==
          Catch::Approx((2.2165682f + 1.9198622f) * 0.5f));
    CHECK(dn::sampleCurve(dn::kLightPhi, 0.0f) == Catch::Approx(2.2165682f));
    CHECK(dn::sampleCurve(dn::kLightPhi, 1.0f) == Catch::Approx(2.2165682f));
    // The sun's curve holds below the horizon overnight: its last key and its
    // first are both 100 degrees.
    CHECK(dn::sampleCurve(dn::kSunPhi, 0.0f) == Catch::Approx(1.7453293f));
}

TEST_CASE("the light always comes down out of the north-west", "[daynight]") {
    for (int minute = 0; minute < 1440; ++minute) {
        const glm::vec3 d = dn::directionalLightDir(minute / 1440.0f);
        INFO("minute " << minute);
        CHECK(glm::length(d) == Catch::Approx(1.0f));
        // Travelling downward: never flat, never from below the ground.
        CHECK(d.z < -0.3f);
        // Toward the south-east in both horizontal axes, equally.
        CHECK(d.x < 0.0f);
        CHECK(d.y < 0.0f);
        CHECK(d.x == Catch::Approx(d.y));
    }
}

TEST_CASE("the light is 37 degrees up at noon and midnight, 20 at six", "[daynight]") {
    CHECK(elevationDeg(-dn::directionalLightDir(0.5f)) == Catch::Approx(37.0f).margin(1.0f));
    CHECK(elevationDeg(-dn::directionalLightDir(0.0f)) == Catch::Approx(37.0f).margin(1.0f));
    CHECK(elevationDeg(-dn::directionalLightDir(0.25f)) == Catch::Approx(20.0f).margin(1.0f));
    CHECK(elevationDeg(-dn::directionalLightDir(0.75f)) == Catch::Approx(20.0f).margin(1.0f));
}

TEST_CASE("the sun crosses the north-west sky on its own curve", "[daynight]") {
    // Five degrees from straight up at noon.
    CHECK(elevationDeg(dn::sunDirection(0.5f)) == Catch::Approx(85.0f).margin(1.5f));
    // Ten below the horizon at 05:30 and 21:30, and below it overnight.
    CHECK(elevationDeg(dn::sunDirection(5.5f / 24.0f)) == Catch::Approx(-10.0f).margin(1.5f));
    CHECK(elevationDeg(dn::sunDirection(21.5f / 24.0f)) == Catch::Approx(-10.0f).margin(1.5f));
    CHECK(dn::sunDirection(0.0f).z < 0.0f);
    CHECK(dn::sunDirection(3.0f / 24.0f).z < 0.0f);
    // Up through the evening the old 5-19 h window cut off.
    CHECK(dn::sunDirection(7.0f / 24.0f).z > 0.0f);
    CHECK(dn::sunDirection(20.0f / 24.0f).z > 0.0f);
    // On the side the light comes from.
    const glm::vec3 sun = dn::sunDirection(0.6f);
    const glm::vec3 from = -dn::directionalLightDir(0.6f);
    CHECK(sun.x * from.x + sun.y * from.y > 0.0f);
}

TEST_CASE("the moon is up around midnight and down by day", "[daynight]") {
    CHECK(elevationDeg(dn::moonDirection(0.0f)) == Catch::Approx(55.0f).margin(2.0f));
    CHECK(dn::moonDirection(23.0f / 24.0f).z > 0.0f);
    CHECK(dn::moonDirection(3.0f / 24.0f).z > 0.0f);
    CHECK(dn::moonDirection(4.0f / 24.0f).z < 0.0f);
    CHECK(dn::moonDirection(0.5f).z < 0.0f);
    CHECK(dn::moonDirection(22.0f / 24.0f).z < 0.0f);
}

TEST_CASE("a light fades linearly from its inner radius to its outer", "[daynight]") {
    CHECK(dn::lightFalloffWeight(5.0f, 10.0f, 20.0f) == 1.0f);
    CHECK(dn::lightFalloffWeight(10.0f, 10.0f, 20.0f) == 1.0f);
    // Linear, where smoothstep gave 0.5 here too but 0.84 at a quarter.
    CHECK(dn::lightFalloffWeight(15.0f, 10.0f, 20.0f) == Catch::Approx(0.5f));
    CHECK(dn::lightFalloffWeight(12.5f, 10.0f, 20.0f) == Catch::Approx(0.75f));
    CHECK(dn::lightFalloffWeight(20.0f, 10.0f, 20.0f) == 0.0f);
    CHECK(dn::lightFalloffWeight(25.0f, 10.0f, 20.0f) == 0.0f);
    // A hard edge: nothing past an inner radius equal to the outer.
    CHECK(dn::lightFalloffWeight(11.0f, 10.0f, 10.0f) == 0.0f);
}

TEST_CASE("the farther light goes on first, so the nearer shows", "[daynight]") {
    const glm::vec3 a(0.0f), b(100.0f, 0.0f, 0.0f);
    CHECK(dn::lightAppliedBefore(400.0f, 10.0f, a, 100.0f, 10.0f, b));
    CHECK_FALSE(dn::lightAppliedBefore(100.0f, 10.0f, b, 400.0f, 10.0f, a));
    // Two lights at the same spot: the larger inner radius first, so the
    // tighter one is laid over it.
    const glm::vec3 c(0.1f, 0.0f, 0.0f);
    CHECK(dn::lightAppliedBefore(100.0f, 50.0f, a, 100.0f, 20.0f, c));
    CHECK_FALSE(dn::lightAppliedBefore(100.0f, 20.0f, c, 100.0f, 50.0f, a));
}

TEST_CASE("each light is lerped over the ones before it, unnormalised", "[daynight]") {
    using wowee::rendering::LightingParams;
    using wowee::rendering::lerpLightingParams;
    LightingParams base, far, near;
    base.ambientColor = glm::vec3(0.0f);
    far.ambientColor = glm::vec3(1.0f, 0.0f, 0.0f);
    near.ambientColor = glm::vec3(0.0f, 1.0f, 0.0f);
    // Half of the far light over the default, then a quarter of the near one.
    LightingParams acc = lerpLightingParams(base, far, 0.5f);
    acc = lerpLightingParams(acc, near, 0.25f);
    CHECK(acc.ambientColor.r == Catch::Approx(0.375f));
    CHECK(acc.ambientColor.g == Catch::Approx(0.25f));
    // What is left is the default showing through, which renormalising two
    // weights to sum to one used to hide.
    CHECK(acc.ambientColor.b == Catch::Approx(0.0f));
    // A light at weight zero leaves everything as it was.
    const LightingParams same = lerpLightingParams(acc, far, 0.0f);
    CHECK(same.ambientColor == acc.ambientColor);
}

TEST_CASE("weather blends the storm sets in by four times its intensity", "[daynight]") {
    CHECK(dn::stormBlend(0.0f) == 0.0f);
    CHECK(dn::stormBlend(0.1f) == Catch::Approx(0.4f));
    CHECK(dn::stormBlend(0.25f) == 1.0f);
    CHECK(dn::stormBlend(0.8f) == 1.0f);
    CHECK(dn::stormBlend(-1.0f) == 0.0f);
}

TEST_CASE("the fog ends inside the far clip at the authored fraction", "[daynight]") {
    const dn::FogRange near = dn::clientFogRange(300.0f, 0.25f, 1000.0f);
    CHECK(near.end == Catch::Approx(300.0f));
    CHECK(near.start == Catch::Approx(75.0f));
    // Past the far clip, the far clip.
    const dn::FogRange clipped = dn::clientFogRange(1500.0f, 0.25f, 1000.0f);
    CHECK(clipped.end == Catch::Approx(1000.0f));
    CHECK(clipped.start == Catch::Approx(250.0f));
    // Never nearer than ten yards, and the fraction kept to [-1, 1].
    CHECK(dn::clientFogRange(5.0f, 0.5f, 1000.0f).end == Catch::Approx(10.0f));
    CHECK(dn::clientFogRange(100.0f, 3.0f, 1000.0f).start == Catch::Approx(100.0f));
    CHECK(dn::clientFogRange(100.0f, -2.0f, 1000.0f).start == Catch::Approx(-100.0f));
}

TEST_CASE("the sky dome's bands sit low over the horizon", "[daynight][sky]") {
    // Seen from the eye, the rows are at 90, 16.8, 9.8, 3.7, 1.8 and 0 degrees
    // of elevation - the fog colour exactly at the horizon.
    const float expectedDeg[] = {90.0f, 16.8f, 9.8f, 3.7f, 1.8f, 0.0f};
    for (int row = 0; row < 6; ++row) {
        const float elev = glm::radians(expectedDeg[row]);
        const glm::vec3 dir(std::cos(elev), 0.0f, std::sin(elev));
        INFO("row " << row);
        CHECK(dn::skyDomePolarFraction(dir) == Catch::Approx(dn::kSkyDomeRows[row]).margin(0.002f));
    }
    // Straight down is the nadir, the last row.
    CHECK(dn::skyDomePolarFraction(glm::vec3(0.0f, 0.0f, -1.0f)) == Catch::Approx(1.0f));
}

TEST_CASE("Light.dbc's parameter columns are named for what the client reads", "[daynight][dbc-layout]") {
    // 0 normal, 1 underwater, 2 storm, 3 storm underwater, 4 death, from
    // column 7 (0x007eb180 indexes them from the record's +0x1c). Column 8
    // was named rain and 9 underwater, which swapped the two in game.
    for (const char* expansion : {"classic", "tbc", "turtle", "wotlk"}) {
        const std::string json = wowee::test::slurp(
            std::string("Data/expansions/") + expansion + "/dbc_layouts.json");
        INFO(expansion);
        REQUIRE(json.size() > 100);
        const size_t at = json.find("\"Light\": {");
        REQUIRE(at != std::string::npos);
        const std::string body = json.substr(at, json.find('}', at) - at);
        const auto column = [&](const char* name) {
            std::smatch m;
            const std::regex re(std::string("\"") + name + R"(\"\s*:\s*(\d+))");
            return std::regex_search(body, m, re) ? std::stoi(m[1].str()) : -1;
        };
        CHECK(column("LightParamsID") == 7);
        CHECK(column("LightParamsIDUnderwater") == 8);
        CHECK(column("LightParamsIDStorm") == 9);
        CHECK(column("LightParamsIDStormUnderwater") == 10);
        CHECK(column("LightParamsIDDeath") == 11);
        CHECK(column("LightParamsIDRain") == -1);
    }
}
