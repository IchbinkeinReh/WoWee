// The ray traced water's optics (rt_water.hpp, water_rt.glsli).
#include <catch_amalgamated.hpp>
#include "rendering/rt_water.hpp"
#include "test_support.hpp"

#include <cmath>
#include <string>

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

TEST_CASE("RT water: the sun's path to the bed stays short however low it stands", "[rt_water]") {
    // Overhead: straight down.
    CHECK(sunPathInWater(2.0f, glm::vec3(0.0f, 0.0f, 1.0f)) == Catch::Approx(2.0f));
    // On the horizon it still enters at no more than the critical angle,
    // 48.6 degrees from the vertical: 2 / cos(48.6).
    const glm::vec3 low = glm::normalize(glm::vec3(1.0f, 0.0f, 0.01f));
    CHECK(sunPathInWater(2.0f, low) == Catch::Approx(3.02f).margin(0.03f));
}

// Stonebull Lake, the camera 5.5 yards back at 20.76 degrees: the shallows in
// front of the character were drawn as deep water over a bed shown bare, and
// the shore's wet band and fade were squeezed into the waterline, with a dark
// line along it where the refraction started under the bed. The shore's masks
// went by the screen's depth, which is the bed's depth where the view's ray
// meets it.
TEST_CASE("RT water: the screen's depth overstates a beach seen from a low camera", "[rt][water][shore]") {
    // Looking down at about 22 degrees over a beach falling at 0.3.
    const float viewSlope = std::tan(glm::radians(22.0f));
    CHECK(screenShoreDepth(0.1f, viewSlope, 0.3f) > 0.35f);
    // Exact over a flat bed, and close looking almost straight down.
    CHECK(screenShoreDepth(0.3f, viewSlope, 0.0f) == Catch::Approx(0.3f));
    CHECK(screenShoreDepth(0.3f, std::tan(glm::radians(80.0f)), 0.25f) < 0.32f);
}

TEST_CASE("RT water: the shore's masks go by the bed straight down", "[rt][water][shore]") {
    const std::string frag = wowee::test::slurp("assets/shaders/water.frag.glsl");
    const std::string rt = wowee::test::slurp("assets/shaders/water_rt.glsli");
    REQUIRE(frag.size() > 1000);
    REQUIRE(rt.size() > 1000);
    // One ray down, the ground and buildings only.
    CHECK(rt.find("float rtwDepthBelow(vec3 p, float reach, inout uint rng)") != std::string::npos);
    CHECK(rt.find("vec3(0.0, 0.0, -1.0)") != std::string::npos);
    // Asked before the refraction starts half of it under the surface, and
    // not overruled by the refracted hit's depth, which is deeper on a beach.
    const size_t asked = frag.find("float under = rtwDepthBelow(FragPos, RTW_SHORE_DEPTH_REACH, rng);");
    const size_t refraction = frag.find("rtwRefraction(FragPos - n * min(0.05, 0.5 * shoreDepth)");
    REQUIRE(asked != std::string::npos);
    REQUIRE(refraction != std::string::npos);
    CHECK(asked < refraction);
    CHECK(frag.find("if (!depthBelowKnown) shoreDepth = max(shoreDepth, rtwBedDepth);") != std::string::npos);
}

TEST_CASE("RT water: the shaded normal faces up, whatever the detail's share", "[rt][water][glint]") {
    // Still water: the waves' tangent and binormal along X and Y.
    const glm::vec3 tangent(1.0f, 0.0f, 0.0f);
    const glm::vec3 binormal(0.0f, 1.0f, 0.0f);
    const glm::vec3 up = waveNormalUp(tangent, binormal);
    CHECK(up.z == Catch::Approx(1.0f));
    // A wave sloping up along +X: its normal leans back toward -X.
    const glm::vec3 slope = waveNormalUp(glm::vec3(1.0f, 0.0f, 0.2f), binormal);
    CHECK(slope.x < -0.1f);
    CHECK(slope.z > 0.9f);
    // The detail normal at the physics waves' calm share (0.55 x 0.25) and at
    // none (past its fade): the normal still faces up. Left facing down, the
    // cross product mixed with an upward detail at less than half its say
    // came out facing down (z < 0), and NdotL killed the glint.
    const glm::vec3 detail = glm::normalize(glm::vec3(0.05f, -0.03f, 1.0f));
    for (float share : {0.0f, 0.55f * 0.25f, 0.55f}) {
        CHECK(shadedNormal(up, detail, share).z > 0.99f);
    }
    const glm::vec3 down = glm::normalize(glm::cross(binormal, tangent));
    CHECK(glm::normalize(glm::mix(down, detail, 0.55f * 0.25f)).z < 0.0f);
}

TEST_CASE("RT water: the sun's glint on calm physics waves, at the mirror angle", "[rt][water][glint]") {
    // The sun 40 degrees up; the view from the mirror direction.
    const glm::vec3 toSun = glm::normalize(glm::vec3(std::cos(glm::radians(40.0f)), 0.0f, std::sin(glm::radians(40.0f))));
    const glm::vec3 toEye(-toSun.x, -toSun.y, toSun.z);
    const glm::vec3 n = shadedNormal(waveNormalUp(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                                     glm::vec3(0.0f, 0.0f, 1.0f), 0.55f * 0.25f);
    const glm::vec3 h = glm::normalize(toSun + toEye);
    CHECK(glm::dot(n, toSun) > 0.6f);
    CHECK(ggx(n, h, 0.18f) > 100.0f);
}

TEST_CASE("RT water: the glint is the sun's by day and the moon's by night", "[rt][water][glint]") {
    const glm::vec3 sunHigh = glm::normalize(glm::vec3(0.3f, 0.2f, 0.9f));
    const glm::vec3 sunDown = glm::normalize(glm::vec3(0.3f, 0.2f, -0.6f));
    const glm::vec3 moonUp = glm::normalize(glm::vec3(-0.5f, 0.4f, 0.5f));
    const glm::vec3 moonDown = glm::normalize(glm::vec3(-0.5f, 0.4f, -0.5f));
    const GlintBody day = glintBody(sunHigh, moonDown);
    CHECK(day.strength == Catch::Approx(1.0f));
    CHECK(glm::dot(day.dir, sunHigh) == Catch::Approx(1.0f));
    const GlintBody night = glintBody(sunDown, moonUp);
    CHECK(night.strength == Catch::Approx(1.0f));
    CHECK(glm::dot(night.dir, moonUp) == Catch::Approx(1.0f));
    CHECK(glintBody(sunDown, moonDown).strength == 0.0f);
    // Rising: in by degrees, not switched on.
    const float rising = glintBody(glm::normalize(glm::vec3(1.0f, 0.0f, 0.015f)), moonDown).strength;
    CHECK(rising > 0.0f);
    CHECK(rising < 1.0f);
}

TEST_CASE("RT water: the shader turns the waves' normal up and mirrors the glint's body", "[rt][water][glint]") {
    const std::string frag = wowee::test::slurp("assets/shaders/water.frag.glsl");
    const std::string frame = wowee::test::slurp("assets/shaders/water_frame.glsli");
    REQUIRE(frag.size() > 1000);
    const size_t turned = frag.find("if (meshNorm.z < 0.0) meshNorm = -meshNorm;");
    const size_t mixed = frag.find("vec3 norm = normalize(mix(meshNorm, detailNorm");
    REQUIRE(turned != std::string::npos);
    REQUIRE(mixed != std::string::npos);
    CHECK(turned < mixed);
    CHECK(frag.find("vec3 glintDir = normalize(rtGlint.xyz);") != std::string::npos);
    CHECK(frag.find("specular = min(specular, vec3(2.0)) * rtGlint.w;") != std::string::npos);
    CHECK(frame.find("vec4 rtGlint;") != std::string::npos);
}
