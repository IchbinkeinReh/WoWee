// The storey a building's own light shades (rt_light_storey.hpp): from the
// floor straight below it to the face straight above it. The hall's hearth
// and the lamps under the hall's ceiling keep to the hall, and the rooms
// upstairs - whose walls face them through the floorboards - keep their
// colours as painted, while the rooms of one floor still share their lamps.
// A storey found too tall darkens a room the light never lit (the Goldshire
// inn's upper floor); one found too low takes a light's shadow off its own
// room.
#include <catch_amalgamated.hpp>

#include "rendering/rt_light_storey.hpp"
#include "rendering/rt_point_lights.hpp"

#include <cmath>
#include <vector>

using namespace wowee::rendering::rt_light_storey;
namespace rt_lights = wowee::rendering::rt_lights;

namespace {

// A square of side 2*h at height z around (cx, cy), wound to face up or down.
void addQuad(std::vector<glm::vec3>& v, std::vector<uint16_t>& idx, float cx, float cy, float h, float z,
             bool up) {
    const uint16_t base = static_cast<uint16_t>(v.size());
    v.push_back({cx - h, cy - h, z});
    v.push_back({cx + h, cy - h, z});
    v.push_back({cx + h, cy + h, z});
    v.push_back({cx - h, cy + h, z});
    if (up) {
        idx.insert(idx.end(), {uint16_t(base), uint16_t(base + 1), uint16_t(base + 2), uint16_t(base),
                               uint16_t(base + 2), uint16_t(base + 3)});
    } else {
        idx.insert(idx.end(), {uint16_t(base), uint16_t(base + 2), uint16_t(base + 1), uint16_t(base),
                               uint16_t(base + 3), uint16_t(base + 2)});
    }
}

// The inn as its file has it, in the parts that matter (model space, its
// lights' and faces' heights from goldshireinn.wmo): the hall (group 5) with
// its floor at 0.43 and its ceiling's underside at 5.7, the rooms upstairs
// with their floor at 7.29 and their ceiling's underside at 15.4, the roof's
// top above; the ground outside the door, and nothing beside the building.
struct Inn {
    std::vector<glm::vec3> hallV, roomV, shellV;
    std::vector<uint16_t> hallI, roomI, shellI;
    std::vector<GroupMesh> groups;
    Inn() {
        addQuad(hallV, hallI, 0.0f, 0.0f, 12.0f, 0.43f, true);
        addQuad(hallV, hallI, 0.0f, 0.0f, 12.0f, 5.7f, false);
        addQuad(roomV, roomI, 0.0f, 0.0f, 12.0f, 7.29f, true);
        addQuad(roomV, roomI, 0.0f, 0.0f, 12.0f, 15.4f, false);
        addQuad(shellV, shellI, 0.0f, 0.0f, 12.0f, 16.76f, true);
        addQuad(shellV, shellI, 40.0f, 0.0f, 10.0f, 0.0f, true);
        groups.push_back({&shellV, &shellI});
        groups.push_back({&hallV, &hallI});
        groups.push_back({&roomV, &roomI});
    }
};

}  // namespace

TEST_CASE("A building's light shades its own storey's baked colours only", "[rt_light_storey]") {
    const Inn inn;
    // The hearth, low in the hall: from the hall's floor to its ceiling.
    const auto hearth = storeyAt({0.06f, -6.14f, 2.14f}, inn.groups);
    REQUIRE(hearth);
    CHECK(hearth->floorZ == Catch::Approx(0.43f));
    CHECK(hearth->ceilingZ == Catch::Approx(5.7f));
    // A lamp a yard under the hall's ceiling: the hall's too.
    const auto hallLamp = storeyAt({-9.13f, -1.13f, 4.48f}, inn.groups);
    REQUIRE(hallLamp);
    CHECK(hallLamp->floorZ == Catch::Approx(0.43f));
    CHECK(hallLamp->ceilingZ == Catch::Approx(5.7f));
    // A lamp hung in a room upstairs: its floor, not the hall's below it -
    // the hall's ceiling under that floor faces down and does not count.
    const auto roomLamp = storeyAt({-8.24f, -3.81f, 13.06f}, inn.groups);
    REQUIRE(roomLamp);
    CHECK(roomLamp->floorZ == Catch::Approx(7.29f));
    CHECK(roomLamp->ceilingZ == Catch::Approx(15.4f));

    // The walls upstairs, from their floor to their top, are as painted
    // under the hall's lights; the hall's own walls still take their shadow.
    for (float z : {7.29f, 8.0f, 11.0f, 15.0f}) {
        CHECK_FALSE(shades(hearth, z, true));
        CHECK_FALSE(shades(hallLamp, z, true));
        CHECK(shades(roomLamp, z, true));
    }
    for (float z : {0.43f, 2.0f, 5.5f}) {
        CHECK(shades(hearth, z, true));
        CHECK_FALSE(shades(roomLamp, z, true));
    }
    // The floor's own boards and the ceiling's are of the storey.
    CHECK(shades(hearth, 0.43f - 0.5f * kMargin, true));
    CHECK(shades(hearth, 5.7f + 0.5f * kMargin, true));
    // Whatever is not a building's baked colour is shaded as before: a fire,
    // a unit, the ground outside.
    CHECK(shades(hearth, 11.0f, false));
}

TEST_CASE("A light with no floor below it shades every baked colour", "[rt_light_storey]") {
    const Inn inn;
    // Beside the building, nothing below it.
    CHECK_FALSE(storeyAt({0.0f, 50.0f, 3.0f}, inn.groups));
    CHECK(shades(std::nullopt, 11.0f, true));
    // Too far above any floor.
    CHECK_FALSE(floorBelow({0.0f, 0.0f, 16.76f + kMaxReach + 1.0f}, inn.groups));
    // Over the ground outside, with nothing above: an open ceiling.
    const auto outside = storeyAt({40.0f, 0.0f, 3.0f}, inn.groups);
    REQUIRE(outside);
    CHECK(std::isinf(outside->ceilingZ));
    CHECK(shades(outside, 25.0f, true));
}

TEST_CASE("A light standing on its floor still finds it", "[rt_light_storey]") {
    const Inn inn;
    CHECK(*floorBelow({0.0f, 0.0f, 0.43f}, inn.groups) == Catch::Approx(0.43f));
    CHECK(*floorBelow({0.0f, 0.0f, 0.43f - 0.5f * kFloorAbove}, inn.groups) == Catch::Approx(0.43f));
}

TEST_CASE("A floor counts up to sixty degrees from level, a wall never", "[rt_light_storey]") {
    std::vector<glm::vec3> v;
    std::vector<uint16_t> idx;
    // A wall under the light: vertical, so no floor and no ceiling.
    v = {{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 3.0f}};
    idx = {0, 1, 2};
    std::vector<GroupMesh> g{{&v, &idx}};
    CHECK_FALSE(floorBelow({0.0f, 0.0f, 5.0f}, g));
    CHECK_FALSE(ceilingAbove({0.0f, 0.0f, -5.0f}, g));
    // A ramp at 45 degrees, wound to face up: a floor.
    v = {{-2.0f, -2.0f, -2.0f}, {2.0f, -2.0f, -2.0f}, {0.0f, 2.0f, 2.0f}};
    CHECK(floorBelow({0.0f, 0.0f, 5.0f}, g));
    // The same ramp wound to face down: its underside, no floor - but a
    // ceiling for a light below it.
    idx = {0, 2, 1};
    CHECK_FALSE(floorBelow({0.0f, 0.0f, 5.0f}, g));
    CHECK(*ceilingAbove({0.0f, 0.0f, -5.0f}, g) == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("A group's box passes by what it cannot hold", "[rt_light_storey]") {
    Inn inn;
    for (auto& g : inn.groups) {
        g.hasBounds = true;
        g.boundsMin = glm::vec3(1e9f);
        g.boundsMax = glm::vec3(-1e9f);
        for (const auto& v : *g.vertices) {
            g.boundsMin = glm::min(g.boundsMin, v);
            g.boundsMax = glm::max(g.boundsMax, v);
        }
    }
    // The same storeys as without the boxes.
    CHECK(storeyAt({0.06f, -6.14f, 2.14f}, inn.groups)->ceilingZ == Catch::Approx(5.7f));
    CHECK(storeyAt({-8.24f, -3.81f, 13.06f}, inn.groups)->floorZ == Catch::Approx(7.29f));
    // A box that does not hold the light is passed by: without the rooms
    // upstairs, the lamp finds the hall's floor below (the hall's ceiling
    // faces down) and the roof above.
    inn.groups[2].boundsMin = glm::vec3(100.0f);
    inn.groups[2].boundsMax = glm::vec3(101.0f);
    const auto s = storeyAt({-8.24f, -3.81f, 13.06f}, inn.groups);
    REQUIRE(s);
    CHECK(s->floorZ == Catch::Approx(0.43f));
    CHECK(s->ceilingZ == Catch::Approx(16.76f));
}

TEST_CASE("The GPU record carries a building light's storey", "[rt_light_storey]") {
    rt_lights::PointLight l;
    l.position = {1, 2, 3};
    l.attenuationEnd = 7.0f;
    l.baked = true;
    // None: the shader's z says so, and it shades everything.
    CHECK(rt_lights::toGpu(l).storey == glm::vec4(0.0f));
    l.hasStorey = true;
    l.storeyFloor = 57.0f;
    l.storeyCeiling = 62.5f;
    CHECK(rt_lights::toGpu(l).storey == glm::vec4(57.0f, 62.5f, 1.0f, 0.0f));
    // An open ceiling goes up as a large finite height.
    l.storeyCeiling = std::numeric_limits<float>::infinity();
    const glm::vec4 g = rt_lights::toGpu(l).storey;
    CHECK(std::isfinite(g.y));
    CHECK(g.y >= 1e29f);
}

TEST_CASE("The vertical through a light meets each floor and ceiling once", "[rt_light_storey]") {
    const Inn inn;
    // Over the inn: the hall's floor and ceiling, the rooms' floor and
    // ceiling, the roof - each one's two triangles meet on the diagonal, so
    // a point off it is in exactly one of them.
    std::vector<float> heights;
    std::vector<float> ups;
    const auto visit = [&](float z, float up) {
        heights.push_back(z);
        ups.push_back(up);
    };
    detail::facesOnVertical({1.0f, 3.0f, 8.0f}, inn.groups, visit);
    REQUIRE(heights.size() == 5);
    int upward = 0;
    for (float u : ups) upward += u > 0.0f ? 1 : 0;
    CHECK(upward == 3);  // the two floors and the roof's top
}
