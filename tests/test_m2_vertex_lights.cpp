// The client's M2 point lights outside the ray traced lighting
// (m2_vertex_lights.hpp): the 20-yard grid round a model's centre, the four
// nearest kept and lights 1-3 taken from the far end of them (0x00834f60,
// 0x008353d0), and the vertex programs' attenuation 1 / (0.7 d + 0.03 d^2).
#include <catch_amalgamated.hpp>

#include "rendering/m2_vertex_lights.hpp"

using namespace wowee::rendering::m2_vertex_lights;

namespace {
Light at(float x, float y, float z, float r = 1.0f) {
    Light l;
    l.position = glm::vec3(x, y, z);
    l.diffuse = glm::vec3(r, 0.0f, 0.0f);
    return l;
}
}  // namespace

TEST_CASE("Attenuation as the vertex programs take it", "[m2_vertex_lights]") {
    REQUIRE(attenuation(1.0f) == Catch::Approx(1.0f / 0.73f));
    REQUIRE(attenuation(10.0f) == Catch::Approx(1.0f / 10.0f));
}

TEST_CASE("Up to three lights, the nearest left out when four are found", "[m2_vertex_lights]") {
    Grid g;
    g.add(at(101, 100, 0, 1));
    g.add(at(102, 100, 0, 2));
    g.add(at(103, 100, 0, 3));
    auto p = g.pick(glm::vec3(100, 100, 0));
    REQUIRE(p.count == 3);
    REQUIRE(p.lights[0].diffuse.r == 3.0f);  // the far end first
    REQUIRE(p.lights[2].diffuse.r == 1.0f);
    g.add(at(104, 100, 0, 4));
    g.add(at(105, 100, 0, 5));
    p = g.pick(glm::vec3(100, 100, 0));
    REQUIRE(p.count == 3);
    // The four nearest are 1..4; lights 1-3 are 4, 3, 2 - the nearest is not drawn.
    REQUIRE(p.lights[0].diffuse.r == 4.0f);
    REQUIRE(p.lights[1].diffuse.r == 3.0f);
    REQUIRE(p.lights[2].diffuse.r == 2.0f);
}

TEST_CASE("Only the cells round the centre are looked in", "[m2_vertex_lights]") {
    Grid g;
    g.add(at(130, 100, 0));   // the next cell but one: 30 yards off
    REQUIRE(g.pick(glm::vec3(105, 105, 0)).count == 0);
    g.add(at(112, 100, 0));   // the same cell, within the +-10 reach
    REQUIRE(g.pick(glm::vec3(105, 105, 0)).count == 1);
}

TEST_CASE("A vertex facing away takes nothing", "[m2_vertex_lights]") {
    Grid g;
    g.add(at(0, 0, 2));
    const auto p = g.pick(glm::vec3(0));
    REQUIRE(vertexLight(p, glm::vec3(0), glm::vec3(0, 0, -1)).r == 0.0f);
    REQUIRE(vertexLight(p, glm::vec3(0), glm::vec3(0, 0, 1)).r == Catch::Approx(attenuation(2.0f)));
}
