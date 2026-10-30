// The ray traced water's optics (rt_water.hpp, water_rt.glsli).
#include <catch_amalgamated.hpp>
#include "rendering/rt_water.hpp"

using namespace wowee::rendering::rt_water;

TEST_CASE("RT water: Fresnel from 2% straight down to all at grazing", "[rt][water]") {
    CHECK(fresnel(1.0f) == Catch::Approx(kF0));
    CHECK(fresnel(0.0f) == Catch::Approx(1.0f));
    float last = fresnel(1.0f);
    for (float c = 0.95f; c >= 0.0f; c -= 0.05f) {
        const float f = fresnel(c);
        CHECK(f >= last);
        last = f;
    }
}

TEST_CASE("RT water: refraction bends toward the normal going in, and reflects past the critical angle",
          "[rt][water]") {
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    // Into the water at 45 degrees: Snell, sin 45 / 1.333.
    const glm::vec3 in45 = glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f));
    const glm::vec3 t = refractDir(in45, up, 1.0f / kIor);
    REQUIRE(glm::length(t) == Catch::Approx(1.0f));
    CHECK(t.x == Catch::Approx(std::sin(glm::radians(45.0f)) / kIor));
    CHECK(t.z < 0.0f);
    // Straight down goes straight on.
    const glm::vec3 down = refractDir(glm::vec3(0.0f, 0.0f, -1.0f), up, 1.0f / kIor);
    CHECK(down.z == Catch::Approx(-1.0f));
    // Out of the water: past 48.6 degrees there is no way through.
    const glm::vec3 below(0.0f, 0.0f, -1.0f);  // the normal, facing the swimmer
    const glm::vec3 steep = glm::normalize(glm::vec3(std::sin(glm::radians(40.0f)), 0.0f, std::cos(glm::radians(40.0f))));
    const glm::vec3 grazing = glm::normalize(glm::vec3(std::sin(glm::radians(60.0f)), 0.0f, std::cos(glm::radians(60.0f))));
    CHECK(glm::length(refractDir(steep, below, kIor)) == Catch::Approx(1.0f));
    CHECK(glm::length(refractDir(grazing, below, kIor)) == 0.0f);
}

TEST_CASE("RT water: deeper is darker and more the water's own colour", "[rt][water]") {
    const glm::vec3 bed(0.8f, 0.7f, 0.5f);       // sand
    const glm::vec3 water(0.05f, 0.15f, 0.25f);  // the body colour, lit
    const glm::vec3 sigma(0.46f, 0.09f, 0.06f);  // red goes first
    const glm::vec3 shallow = throughWater(bed, water, sigma, 0.2f);
    const glm::vec3 deep = throughWater(bed, water, sigma, 120.0f);  // blue goes last: e^-7
    CHECK(shallow.r > deep.r);
    CHECK(glm::length(shallow - bed) < 0.1f);
    CHECK(glm::length(deep - water) < 0.02f);
    // Red is lost first: bluer with depth.
    const glm::vec3 mid = throughWater(bed, water, sigma, 4.0f);
    CHECK(mid.b / mid.r > bed.b / bed.r);
    // No path, no change.
    CHECK(glm::length(throughWater(bed, water, sigma, 0.0f) - bed) < 1e-6f);
}

TEST_CASE("RT water: the sky from horizon to zenith", "[rt][water]") {
    const glm::vec3 horizon(0.8f, 0.8f, 0.7f), top(0.2f, 0.4f, 0.9f);
    CHECK(glm::length(sky(glm::vec3(1.0f, 0.0f, 0.0f), horizon, top) - horizon) < 1e-6f);
    CHECK(glm::length(sky(glm::vec3(0.0f, 0.0f, 1.0f), horizon, top) - top) < 1e-6f);
    CHECK(glm::length(sky(glm::vec3(1.0f, 0.0f, -0.5f), horizon, top) - horizon) < 1e-6f);
}
