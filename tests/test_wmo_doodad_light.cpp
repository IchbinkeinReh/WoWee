// How the original client (Wow.exe 3.3.5a, build 12340) lights an M2 inside a
// WMO interior: a doodad by its MODD colour, a unit by the floor's vertex
// colour, each split into an ambient and one direct light from a fixed
// direction, with no sun (0x007bef40, 0x007c1ad0, 0x007c1150, 0x007a0d60,
// 0x007c7fe0). WoWee lit them by the outdoor sun and its shadow map.
#include <catch_amalgamated.hpp>

#include "pipeline/wmo_doodad_light.hpp"

namespace wl = wowee::pipeline::wmo_doodad_light;

TEST_CASE("a MODD colour splits into a dim ambient and a bright light", "[wmo][doodad-light]") {
    // Mid grey: the ambient is cut to 96, (96 * 255 / 127 = 192, then
    // (127 * 192 + 255) >> 8 = 96); the light, already past 112, is kept.
    const auto grey = wl::doodadLight(glm::vec4(127.0f / 255.0f, 127.0f / 255.0f, 127.0f / 255.0f, 1.0f));
    CHECK(grey.ambient.r * 255.0f == Catch::Approx(96.0f));
    CHECK(grey.direct.r * 255.0f == Catch::Approx(127.0f));

    // A dim warm colour: the ambient is kept, the light raised to 112 in
    // value - every channel by the same factor, so the hue holds.
    const auto dim = wl::doodadLight(glm::vec4(56.0f / 255.0f, 28.0f / 255.0f, 14.0f / 255.0f, 1.0f));
    CHECK(dim.ambient.r * 255.0f == Catch::Approx(56.0f));
    CHECK(dim.direct.r * 255.0f == Catch::Approx(112.0f));
    CHECK(dim.direct.g * 255.0f == Catch::Approx(56.0f));
    CHECK(dim.direct.b * 255.0f == Catch::Approx(28.0f));

    // Black stays black: there is no hue to raise.
    const auto black = wl::doodadLight(glm::vec4(0.0f));
    CHECK(black.ambient.r == 0.0f);
    CHECK(black.direct.r == 0.0f);

    // A unit's light is raised further, to 168 (0x007a0d60 passes 0xa8).
    const auto unit = wl::unitLight(glm::ivec3(84, 42, 0));
    CHECK(unit.direct.r * 255.0f == Catch::Approx(168.0f));
    CHECK(unit.direct.g * 255.0f == Catch::Approx(84.0f));
    CHECK(unit.ambient.r * 255.0f == Catch::Approx(84.0f));

    // The light comes down out of the +x +y quarter (0xaeedf0).
    CHECK(glm::length(wl::kInteriorLightDir) == Catch::Approx(1.0f).margin(0.001f));
    CHECK(wl::kInteriorLightDir.z < -0.85f);
}

TEST_CASE("a unit takes the floor's vertex colour doubled", "[wmo][doodad-light]") {
    // 0x007c7fe0: twice the MOCV, plus the WMO's ambient when MOHD flag 0x2
    // is set, each channel at most 255.
    const glm::ivec3 plain = wl::unitFloorColor(glm::vec3(40.0f, 100.0f, 200.0f), 0u, glm::vec4(0.5f));
    CHECK(plain == glm::ivec3(80, 200, 255));
    const glm::ivec3 lit = wl::unitFloorColor(glm::vec3(40.0f, 10.0f, 0.0f), 0x2u,
                                              glm::vec4(20.0f / 255.0f, 0.0f, 0.0f, 1.0f));
    CHECK(lit == glm::ivec3(100, 20, 0));
}

TEST_CASE("only doodads that interior groups alone list are interior-lit", "[wmo][doodad-light]") {
    wowee::pipeline::WMOModel model;
    model.doodads.resize(5);
    model.groups.resize(3);
    model.groups[0].flags = 0x2000;  // interior
    model.groups[0].doodadRefs = {0, 1, 3};
    model.groups[1].flags = 0x8;     // exterior
    model.groups[1].doodadRefs = {1, 2};
    model.groups[2].flags = 0x40;    // exterior-lit
    model.groups[2].doodadRefs = {3, 99};
    const auto interior = wl::interiorDoodads(model);
    REQUIRE(interior.size() == 5);
    CHECK(interior[0] == 1);  // interior alone
    CHECK(interior[1] == 0);  // an exterior group lists it too, and that wins
    CHECK(interior[2] == 0);  // exterior
    CHECK(interior[3] == 0);  // exterior-lit
    CHECK(interior[4] == 0);  // no group lists it
}
