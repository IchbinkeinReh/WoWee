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

TEST_CASE("a group's vertex colours as the client keeps them once loaded", "[wmo][doodad-light]") {
    // 0x007d7380: a transition batch's vertex is halved and keeps its alpha.
    CHECK(wl::loadedVertexColor(glm::ivec4(200, 101, 0, 77), true) == glm::ivec4(100, 50, 0, 77));
    // The others: ((c * a >> 6) + c) >> 1, at most 255, and alpha 255. At
    // alpha 0 that is half the colour, which 0x007c7fe0 doubles back.
    CHECK(wl::loadedVertexColor(glm::ivec4(200, 100, 10, 0), false) == glm::ivec4(100, 50, 5, 255));
    CHECK(wl::loadedVertexColor(glm::ivec4(100, 40, 0, 64), false) == glm::ivec4(100, 40, 0, 255));
    CHECK(wl::loadedVertexColor(glm::ivec4(200, 100, 0, 255), false) == glm::ivec4(255, 249, 0, 255));
}

TEST_CASE("a transition face carries a floor's light toward the zone's", "[wmo][doodad-light]") {
    // 0x006acc50 as 0x007a0d60 uses it: 255 is all the way, otherwise the
    // alpha is of 256.
    const glm::vec3 a(0.2f, 0.4f, 0.6f);
    const glm::vec3 b(1.0f, 0.0f, 0.6f);
    CHECK(wl::towardColor(a, b, 255) == b);
    CHECK(wl::towardColor(a, b, 0) == a);
    const glm::vec3 half = wl::towardColor(a, b, 128);
    CHECK(half.r == Catch::Approx(0.6f));
    CHECK(half.g == Catch::Approx(0.2f));

    wl::FloorLight floor;
    floor.light.ambient = glm::vec3(0.3f);
    floor.light.direct = glm::vec3(0.7f);
    const glm::vec3 zoneAmbient(0.5f, 0.5f, 0.6f);
    const glm::vec3 zoneDirect(1.0f, 0.9f, 0.8f);

    // Off a transition face the floor's light as it is, from the interior
    // direction.
    const auto inside = wl::floorLightInZone(floor, zoneAmbient, zoneDirect);
    CHECK(inside.ambient == floor.light.ambient);
    CHECK(inside.direct == floor.light.direct);
    CHECK(inside.towardSun == 0.0f);

    // At the doorway's outer edge the zone's light, from the sun
    // (0x007c1730 turns the direction by alpha / 255).
    floor.towardOutside = 255;
    const auto outside = wl::floorLightInZone(floor, zoneAmbient, zoneDirect);
    CHECK(outside.ambient == zoneAmbient);
    CHECK(outside.direct == zoneDirect);
    CHECK(outside.towardSun == 1.0f);

    floor.towardOutside = 51;
    const auto partway = wl::floorLightInZone(floor, zoneAmbient, zoneDirect);
    CHECK(partway.ambient.r == Catch::Approx(0.3f + 0.2f * 51.0f / 256.0f));
    CHECK(partway.towardSun == Catch::Approx(0.2f));
}

TEST_CASE("a unit's ambient eases between floors", "[wmo][doodad-light]") {
    // 0x007a1e90: each channel moves toward its target by seconds x 510, at
    // least 1, and stops on it.
    const glm::ivec3 from(40, 200, 100);
    const glm::ivec3 to(140, 100, 100);
    CHECK(wl::easeAmbient(from, to, 0.1f) == glm::ivec3(91, 149, 100));
    CHECK(wl::easeAmbient(from, to, 0.0f) == glm::ivec3(41, 199, 100));
    CHECK(wl::easeAmbient(from, to, 1.0f) == to);
    // A full swing is over in about half a second: 8 a frame at 60 a second.
    glm::ivec3 c(0);
    for (int frame = 0; frame < 32; ++frame) c = wl::easeAmbient(c, glm::ivec3(255), 1.0f / 60.0f);
    CHECK(c == glm::ivec3(255));
}

TEST_CASE("a group's drawn vertex colours are the ones the client fixed up", "[wmo][doodad-light]") {
    // 0x007d7c30 runs 0x007d7380 over a group with MOCV (MOGP 0x4) unless
    // MOHD flag 0x8 is set; the transition batches' vertices are those up to
    // the last transition batch's last vertex.
    wowee::pipeline::WMOGroup group{};
    group.flags = 0x4;
    group.transBatchCount = 1;
    wowee::pipeline::WMOBatch transition{};
    transition.lastVertex = 0;
    wowee::pipeline::WMOBatch rest{};
    rest.lastVertex = 1;
    group.batches = {transition, rest};
    group.vertices.resize(2);
    group.vertices[0].color = glm::vec4(200.0f, 100.0f, 0.0f, 128.0f) / 255.0f;
    group.vertices[1].color = glm::vec4(100.0f, 40.0f, 0.0f, 64.0f) / 255.0f;
    CHECK(wl::transitionVertexEnd(group) == 1);

    const auto fixed = wl::loadedVertexColors(group, 0u);
    REQUIRE(fixed.size() == 2);
    CHECK(glm::ivec4(glm::round(fixed[0] * 255.0f)) == glm::ivec4(100, 50, 0, 128));
    CHECK(glm::ivec4(glm::round(fixed[1] * 255.0f)) == glm::ivec4(100, 40, 0, 255));

    // MOHD flag 0x8: as the MOCV has them.
    const auto raw = wl::loadedVertexColors(group, 0x8u);
    CHECK(glm::ivec4(glm::round(raw[0] * 255.0f)) == glm::ivec4(200, 100, 0, 128));

    // No MOCV: nothing to fix, the loader's white stays.
    group.flags = 0;
    group.vertices[1].color = glm::vec4(1.0f);
    CHECK(wl::loadedVertexColors(group, 0u)[1] == glm::vec4(1.0f));
    group.transBatchCount = 0;
    CHECK(wl::transitionVertexEnd(group) == 0);
}

TEST_CASE("a WMO batch's fog follows 0x007a9380", "[wmo][doodad-light][fog]") {
    using F = wl::BatchFog;
    // Exterior and exterior-lit groups: the zone's colour, F_UNFOGGED or not.
    CHECK(wl::batchFogs(0x8, false, 0x2, true).insidePass == F::Zone);
    CHECK(wl::batchFogs(0x40, false, 0x0, true).insidePass == F::Zone);
    // An interior group: the camera's in the camera's interior pass, the
    // zone's otherwise; F_UNFOGGED is not read off a transition batch.
    CHECK(wl::batchFogs(0x2000, false, 0x2, true).insidePass == F::Camera);
    CHECK(wl::batchFogs(0x2000, false, 0x0, false).insidePass == F::Zone);
    // A transition batch: the outside pass in the zone's fog, the inside pass
    // in the group's; F_UNFOGGED takes both away.
    const auto t = wl::batchFogs(0x2000, true, 0x0, true);
    CHECK(t.outsidePass == F::Zone);
    CHECK(t.insidePass == F::Camera);
    CHECK(wl::batchFogs(0x2000, true, 0x0, false).insidePass == F::Zone);
    const auto u = wl::batchFogs(0x2000, true, 0x2, true);
    CHECK(u.outsidePass == F::None);
    CHECK(u.insidePass == F::None);
}

TEST_CASE("the camera's interior pass stops at exterior groups", "[wmo][doodad-light][fog]") {
    // 0 (interior, camera) - 1 (interior) - 2 (exterior) - 3 (interior);
    // 4 is interior but always-draw (0x10000), 5 interior and unreachable.
    const std::vector<uint32_t> flags = {0x2000, 0x2000, 0x8, 0x2000, 0x12000, 0x2000};
    const std::vector<std::vector<uint32_t>> next = {{1, 4}, {0, 2}, {1, 3}, {2}, {0}, {}};
    const auto pass = wl::interiorPassGroups(flags, next, {0});
    CHECK(pass == std::vector<uint8_t>{1, 1, 0, 0, 0, 0});
    // A camera in an exterior group starts no pass (0x007ac060 clears it).
    CHECK(wl::interiorPassGroups(flags, next, {2}) == std::vector<uint8_t>(6, 0));
    // Exterior-lit counts as outside too.
    CHECK(wl::interiorPassGroups({0x2040, 0x2000}, {{1}, {0}}, {0}) == std::vector<uint8_t>{0, 0});
}

TEST_CASE("a world object's floor search", "[wmo][doodad-light][floor]") {
    // 0x007c2a70: a unit's from a tenth of a yard up; 0x007c2e70: a game
    // object's from four yards up, or a tenth over its top if lower.
    CHECK(wl::kUnitFloorAbove == Catch::Approx(0.1f));
    CHECK(wl::gameObjectFloorStart(10.0f, 20.0f) == Catch::Approx(14.0f));
    CHECK(wl::gameObjectFloorStart(10.0f, 11.0f) == Catch::Approx(11.1f));

    // 0x007c28f0: the ground wins when it is nearer than the floor.
    CHECK(wl::terrainNearer(10.0f, 9.0f, 5.0f));
    CHECK_FALSE(wl::terrainNearer(10.0f, 4.0f, 5.0f));
    // Ground above the start, or none, is not counted.
    CHECK_FALSE(wl::terrainNearer(10.0f, 12.0f, 5.0f));
    CHECK_FALSE(wl::terrainNearer(10.0f, std::nullopt, 5.0f));
}

TEST_CASE("a world object's direct light in the terrain's baked shadow", "[wmo][doodad-light][shadow]") {
    // 0x007a1bc0: 2.5 outside, 0.5 in the baked shadow, 1 on an interior
    // floor, 1 + alpha x 1.5 on a transition face.
    CHECK(wl::directScaleTarget(false, 0, false) == Catch::Approx(2.5f));
    CHECK(wl::directScaleTarget(false, 0, true) == Catch::Approx(0.5f));
    CHECK(wl::directScaleTarget(true, 0, true) == Catch::Approx(1.0f));
    CHECK(wl::directScaleTarget(true, 255, false) == Catch::Approx(2.5f));

    // 0x007a1e90: 3.33 a second toward it, and never above 1.
    CHECK(wl::easeDirectScale(1.0f, 2.5f, 0.1f) == Catch::Approx(1.0f));
    CHECK(wl::easeDirectScale(1.0f, 0.5f, 0.06f) == Catch::Approx(0.8f));
    CHECK(wl::easeDirectScale(0.6f, 0.5f, 0.06f) == Catch::Approx(0.5f));
    CHECK(wl::easeDirectScale(0.5f, 2.5f, 0.06f) == Catch::Approx(0.7f));
    // Into the shadow takes 0.15 s.
    float s = 1.0f;
    for (int frame = 0; frame < 9; ++frame) s = wl::easeDirectScale(s, 0.5f, 1.0f / 60.0f);
    CHECK(s == Catch::Approx(0.5f));
}
