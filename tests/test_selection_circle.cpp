// The circle under the target (0x00725980, 0x00720330, 0x00521bf0,
// 0x00744150, 0x007e2d60): its radius from the model's box, its colour,
// where its texture lies over the ground and how a fragment is shaded.
#include <catch_amalgamated.hpp>

#include "rendering/selection_circle.hpp"

#include <cmath>

namespace sc = wowee::rendering::selection_circle;
namespace bs = wowee::rendering::blob_shadow;

namespace {
glm::vec2 uvAt(const bs::Projection& p, const glm::vec3& w) {
    const glm::vec4 h(w, 1.0f);
    return {glm::dot(p.uRow, h), glm::dot(p.vRow, h)};
}
float heightAt(const bs::Projection& p, const glm::vec3& w) {
    return glm::dot(p.hRow, glm::vec4(w, 1.0f));
}
bool same(const glm::vec4& a, int r, int g, int b) {
    return a == glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}
}  // namespace

TEST_CASE("the radius is the square root of the scaled half diagonal", "[selection_circle]") {
    // A 0.6 by 0.8 box: half its diagonal is 0.5.
    const bs::Box box{{-0.3f, -0.4f, 0.0f}, {0.3f, 0.4f, 2.0f}};
    CHECK(sc::radius(box, 1.0f) == Catch::Approx(std::sqrt(0.5f)));
    CHECK(sc::radius(box, 2.0f) == Catch::Approx(1.0f));
    // The height plays no part.
    bs::Box tall = box;
    tall.max.z = 9.0f;
    CHECK(sc::radius(tall, 1.0f) == Catch::Approx(sc::radius(box, 1.0f)));
}

TEST_CASE("past 5 the radius grows by 0.06 of the excess squared, held to 10", "[selection_circle]") {
    // Half diagonal 5 at scale 1.4: s = 7.
    const bs::Box box{{-3.0f, -4.0f, 0.0f}, {3.0f, 4.0f, 1.0f}};
    CHECK(sc::radius(box, 1.4f) == Catch::Approx(std::sqrt(7.0f) + 0.06f * 4.0f));
    CHECK(sc::radius(box, 1.0f) == Catch::Approx(std::sqrt(5.0f)));
    CHECK(sc::radius(box, 6.0f) == 10.0f);
}

TEST_CASE("a box with no ground extent gives 1.2", "[selection_circle]") {
    CHECK(sc::radius(bs::Box{}, 1.0f) == Catch::Approx(1.2f));
    CHECK(sc::radius(bs::Box{{0, 0, 0}, {0, 0, 2}}, 3.0f) == Catch::Approx(1.2f));
}

TEST_CASE("a creature's circle is coloured by its reaction, grey when dead", "[selection_circle]") {
    sc::ColorInput in;
    in.reaction = 1;
    CHECK(same(sc::color(in), 255, 0, 0));
    in.reaction = 2;
    CHECK(same(sc::color(in), 255, 0, 0));
    in.reaction = 3;
    CHECK(same(sc::color(in), 255, 128, 0));
    in.reaction = 4;
    CHECK(same(sc::color(in), 255, 255, 0));
    for (int r = 5; r <= 8; ++r) {
        in.reaction = r;
        CHECK(same(sc::color(in), 0, 255, 0));
    }
    in.dead = true;
    CHECK(same(sc::color(in), 127, 127, 127));
}

TEST_CASE("a player's circle follows who may attack whom", "[selection_circle]") {
    sc::ColorInput in;
    in.playerControlled = true;
    in.reaction = 2;
    in.dead = true;  // not read for a player
    in.targetMayAttackPlayer = true;
    in.playerMayAttackTarget = true;
    CHECK(same(sc::color(in), 255, 0, 0));
    in.playerMayAttackTarget = false;
    CHECK(same(sc::color(in), 96, 96, 255));
    in.targetMayAttackPlayer = false;
    in.playerMayAttackTarget = true;
    CHECK(same(sc::color(in), 255, 255, 0));
}

TEST_CASE("a friendly player's circle: group, friends, PvP", "[selection_circle]") {
    sc::ColorInput in;
    in.playerControlled = true;
    in.reaction = 5;
    CHECK(same(sc::color(in), 96, 96, 255));
    in.pvp = true;
    CHECK(same(sc::color(in), 0, 255, 0));
    in.friendListed = true;
    CHECK(same(sc::color(in), 83, 201, 255));
    in.groupMember = true;
    CHECK(same(sc::color(in), 170, 255, 170));
    in.pvp = false;
    CHECK(same(sc::color(in), 170, 170, 255));
}

TEST_CASE("the box is the unit's position and the radius either way", "[selection_circle]") {
    const glm::vec3 unit(100.0f, 200.0f, 30.0f);
    const auto p = sc::project(unit, 0.75f, glm::vec3(90.0f, 200.0f, 35.0f));
    REQUIRE(p);
    CHECK(p->boxMin == unit - glm::vec3(0.75f));
    CHECK(p->boxMax == unit + glm::vec3(0.75f));
    CHECK_FALSE(sc::project(unit, 0.0f, glm::vec3(0.0f)));
}

TEST_CASE("the texture spans the box with its top away from the camera", "[selection_circle]") {
    const glm::vec3 unit(10.0f, -4.0f, 2.0f);
    const float r = 1.5f;
    for (float bearing : {0.0f, 0.7f, 2.0f, -2.5f}) {
        const glm::vec3 away(std::cos(bearing), std::sin(bearing), 0.0f);
        const glm::vec3 camera = unit - away * 12.0f + glm::vec3(0.0f, 0.0f, 5.0f);
        const auto p = sc::project(unit, r, camera);
        REQUIRE(p);
        const glm::vec2 centre = uvAt(*p, unit);
        CHECK(centre.x == Catch::Approx(0.5f));
        CHECK(centre.y == Catch::Approx(0.5f));
        // Far side at the top edge, near side at the bottom.
        const glm::vec2 far = uvAt(*p, unit + away * r);
        CHECK(far.x == Catch::Approx(0.5f).margin(1e-5));
        CHECK(far.y == Catch::Approx(0.0f).margin(1e-5));
        const glm::vec2 near = uvAt(*p, unit - away * r);
        CHECK(near.y == Catch::Approx(1.0f).margin(1e-5));
        // Counter-clockwise from the line of sight, seen from above: u 1.
        const glm::vec3 left(-away.y, away.x, 0.0f);
        const glm::vec2 l = uvAt(*p, unit + left * r);
        CHECK(l.x == Catch::Approx(1.0f).margin(1e-5));
        CHECK(l.y == Catch::Approx(0.5f).margin(1e-5));
        // Height plays no part in the texture.
        const glm::vec2 up = uvAt(*p, unit + glm::vec3(0.0f, 0.0f, 0.6f));
        CHECK(up.x == Catch::Approx(0.5f));
        CHECK(up.y == Catch::Approx(0.5f));
    }
}

TEST_CASE("the camera's bearing reads as 0x004f5130 has it", "[selection_circle]") {
    const glm::vec3 c(0.0f);
    CHECK(sc::facingAngle(c, {0.0f, 2.0f, 0.0f}) == Catch::Approx(0.5f * 3.1415927f));
    CHECK(sc::facingAngle(c, {0.0f, -2.0f, 0.0f}) == Catch::Approx(1.5f * 3.1415927f));
    CHECK(sc::facingAngle(c, {-2.0f, 0.0f, 0.0f}) == Catch::Approx(3.1415927f));
    CHECK(sc::facingAngle(c, {2.0f, 0.0f, 0.0f}) == 0.0f);
    CHECK(sc::facingAngle(c, {1.0f, 1.0f, 0.0f}) == Catch::Approx(0.25f * 3.1415927f));
}

TEST_CASE("the height fade runs over the box's height", "[selection_circle]") {
    const glm::vec3 unit(0.0f, 0.0f, 50.0f);
    const auto p = sc::project(unit, 2.0f, glm::vec3(-5.0f, 0.0f, 55.0f));
    REQUIRE(p);
    CHECK(heightAt(*p, unit) == Catch::Approx(0.5f));
    CHECK(heightAt(*p, unit - glm::vec3(0, 0, 2.0f)) == Catch::Approx(0.0f));
    CHECK(heightAt(*p, unit + glm::vec3(0, 0, 2.0f)) == Catch::Approx(1.0f));
}

TEST_CASE("a fragment is the texel by the colour, its alpha faded at the box's ends", "[selection_circle]") {
    const glm::vec4 texel(0.5f, 1.0f, 0.25f, 0.8f);
    const glm::vec4 red(1.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 mid = sc::shade(texel, red, 0.5f);
    CHECK(mid.r == Catch::Approx(0.5f));
    CHECK(mid.g == 0.0f);
    CHECK(mid.a == Catch::Approx(0.8f));
    // Gone at the floor and the top of the box.
    CHECK(sc::shade(texel, red, 0.0f).a == Catch::Approx(0.0f).margin(0.01f));
    CHECK(sc::shade(texel, red, 1.0f).a == Catch::Approx(0.0f).margin(0.01f));
    // A twelfth of the way up is texel 4.83 of 64, 0.92 of the way along
    // the sixth the fade comes in over: a little under half in.
    CHECK(sc::shade(texel, red, 1.0f / 12.0f).a == Catch::Approx(0.8f * 0.46f).margin(0.01f));
}
