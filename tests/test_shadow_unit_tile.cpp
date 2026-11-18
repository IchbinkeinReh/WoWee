// The units' own tile of the sun's shadow atlas (shadow_unit_tile.hpp,
// shadow_csm.glsli's csmUnitShadow, water_rt.glsli's rtwSunThroughAir).
#include <catch_amalgamated.hpp>
#include "rendering/shadow_unit_tile.hpp"
#include "test_support.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <string>

using namespace wowee::rendering::shadow_unit_tile;

namespace {
// A tile's matrix as the renderer makes it: the sun some 67 degrees up (14:00
// at Stonebull Lake), looking at `center`.
glm::mat4 tileMatrix(const glm::vec3& center) {
    const glm::vec3 sunDir = glm::normalize(glm::vec3(-0.35f, -0.15f, -0.92f));
    const glm::mat4 view = glm::lookAt(center - sunDir * 1500.0f, center, glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::ortho(-kHalfExtent, kHalfExtent, -kHalfExtent, kHalfExtent, 1.0f, 3250.0f);
    proj[1][1] *= -1.0f;
    return proj * view;
}
}  // namespace

TEST_CASE("one cascade over 500 yards gives the units a tile ten times finer", "[shadow][unit_tile]") {
    // The user's settings: one cascade of 4096 over 500 yards, a quarter of a
    // yard a texel - a character a handful of them.
    const float cascadeTexel = 2.0f * 500.0f / 4096.0f;
    REQUIRE(wanted(500.0f, 4096));
    CHECK(side(4096) == 2048u);
    CHECK(texelWorld(side(4096)) < 0.025f);
    CHECK(cascadeTexel / texelWorld(side(4096)) > 10.0f);
    // Larger maps: the tile stays 2048, still well under the cascade.
    CHECK(wanted(500.0f, 16384));
    CHECK(side(16384) == 2048u);
    // The default two cascades at 4096 over 300 yards: the near one, about 50
    // yards, is already 0.025 a texel; no tile.
    CHECK_FALSE(wanted(50.6f, 4096));
    // A small map is coarse even close in, and gets a tile its own size.
    CHECK(wanted(50.6f, 1024));
    CHECK(side(1024) == 1024u);
    CHECK_FALSE(wanted(0.0f, 4096));
    CHECK_FALSE(wanted(500.0f, 0));
}

TEST_CASE("the units' tile goes beside the cascades, or under them, or nowhere", "[shadow][unit_tile]") {
    // One cascade in a device that draws 16384 across: to the right.
    Placement p = place(4096, 4096, 4096, 1, true, 2048, 16384, 16384);
    CHECK(p.placed);
    CHECK(p.x == 4096u);
    CHECK(p.y == 0u);
    CHECK(p.atlasW == 6144u);
    CHECK(p.atlasH == 4096u);
    // No room across: underneath.
    p = place(4096, 4096, 4096, 1, true, 2048, 4096, 8192);
    CHECK(p.placed);
    CHECK(p.x == 0u);
    CHECK(p.y == 4096u);
    CHECK(p.atlasW == 4096u);
    CHECK(p.atlasH == 6144u);
    // Three cascades in two rows: the quarter they leave free, the atlas as it was.
    p = place(8192, 8192, 4096, 3, false, 2048, 8192, 8192);
    CHECK(p.placed);
    CHECK(p.x == 4096u);
    CHECK(p.y == 4096u);
    CHECK(p.atlasW == 8192u);
    CHECK(p.atlasH == 8192u);
    // No room anywhere: the cascades' atlas alone.
    p = place(16384, 16384, 16384, 1, true, 2048, 16384, 16384);
    CHECK_FALSE(p.placed);
    CHECK(p.atlasW == 16384u);
    CHECK(p.atlasH == 16384u);
    // No tile asked for.
    p = place(4096, 4096, 4096, 1, true, 0, 16384, 16384);
    CHECK_FALSE(p.placed);
    CHECK(p.atlasW == 4096u);
}

TEST_CASE("a unit the tile holds whole casts there alone", "[shadow][unit_tile]") {
    const glm::vec3 player(-2177.9f, -726.3f, -15.4f);
    const glm::mat4 m = tileMatrix(player);
    const float r = reach(glm::vec3(-0.5f, -0.5f, 0.0f), glm::vec3(0.5f, 0.5f, 2.2f), 1.0f);
    CHECK(r == Catch::Approx(std::sqrt(0.25f + 0.25f + 2.2f * 2.2f)));
    // The player: whole - in the tile, left out of the cascades.
    CHECK(cover(m, player, r, kHalfExtent) == Cover::Whole);
    // A unit across the tile's edge: in both.
    const glm::vec3 lightRight = glm::normalize(glm::vec3(glm::inverse(m)[0]));
    const glm::vec3 edge = player + lightRight * kHalfExtent;
    CHECK(cover(m, edge, r, kHalfExtent) == Cover::Partly);
    // Far off: the cascades' alone.
    CHECK(cover(m, player + lightRight * (kHalfExtent * 2.0f), r, kHalfExtent) == Cover::Outside);
    // Scaled up, the same box reaches farther.
    CHECK(reach(glm::vec3(-0.5f), glm::vec3(0.5f), 3.0f) == Catch::Approx(3.0f * std::sqrt(0.75f)));
}

TEST_CASE("surfaces take the cascades' shadow times the units' tile's", "[shadow][unit_tile][shader]") {
    const std::string csm = wowee::test::slurp("assets/shaders/shadow_csm.glsli");
    REQUIRE(csm.size() > 1000);
    // The cascades read no more than three slots; the fourth is the tile.
    CHECK(csm.find("const int count = min(int(cascadeInfo.x + 0.5), 3);") != std::string::npos);
    CHECK(csm.find("cascadeTexel[3].w < 0.5") != std::string::npos);
    CHECK(csm.find("return csmCascadeShadow(smap, worldPos, n, nl, depthBias) * csmUnitShadow(smap, worldPos, n, nl, depthBias);") !=
          std::string::npos);
}

TEST_CASE("the water's sun through the air takes the units from the tile", "[shadow][unit_tile][water]") {
    const std::string frag = wowee::test::slurp("assets/shaders/water.frag.glsl");
    const std::string rt = wowee::test::slurp("assets/shaders/water_rt.glsli");
    REQUIRE(frag.size() > 1000);
    REQUIRE(rt.size() > 1000);
    CHECK(frag.find("layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;") != std::string::npos);
    CHECK(frag.find("#include \"shadow_csm.glsli\"") != std::string::npos);
    // The units passed by where the tile holds the point, its shadow instead.
    CHECK(rt.find("rtSkipUnits = skipUnits || byMap;") != std::string::npos);
    CHECK(rt.find("return byMap ? rtwUnitsLit(surface, sunRay, filtered) : 1.0;") != std::string::npos);
    // The bed's air and the surface's own sun both go that way.
    CHECK(rt.find("float air = rtwSunThroughAir(hit + sunUnder * sunPath, sunRay, true, rng);") != std::string::npos);
    CHECK(frag.find("sunOnWater = rtwSunThroughAir(FragPos, sunRay, false, rng);") != std::string::npos);
    // Not the one hard ray at the units any more.
    CHECK(rt.find("bool airBlocked = rtOccluded(") == std::string::npos);
}
