// The client's ribbon emitter (CRibbonEmitter, 0x0097f510 to 0x00980b70).
#include <catch_amalgamated.hpp>

#include "rendering/client_ribbon.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::client_ribbon::Emitter;
using wowee::rendering::client_ribbon::Vertex;

namespace {
glm::mat4 at(const glm::vec3& p) { return glm::translate(glm::mat4(1.0f), p); }
}  // namespace

TEST_CASE("a ribbon rounds its rate up and keeps a quarter second at least", "[ribbon]") {
    Emitter r;
    r.init(7.5f, 0.1f, 1, 1);
    CHECK(r.rate() == 8.0f);
    CHECK(r.lifetime() == 0.25f);
    // A lifetime's edges and two more: ceil(0.25 x 8) + 2.
    CHECK(r.capacity() == 4u);
}

TEST_CASE("edges are laid down along the way the bone moved, across its Y axis", "[ribbon]") {
    Emitter r;
    r.init(10.0f, 1.0f, 1, 1);
    r.setAbove(1.0f);
    r.setBelow(0.5f);
    r.setVisible(true);
    r.setTransform(at({0, 0, 0}));
    r.update(0.0f, false);  // the first update steps one edge interval
    r.setTransform(at({1, 0, 0}));
    r.update(0.1f, false);
    std::vector<Vertex> strip;
    r.appendStrip(strip);
    REQUIRE(strip.size() >= 4);
    // The tip is where the bone is now, its points below and above on Y.
    const Vertex& tipBottom = strip[strip.size() - 2];
    const Vertex& tipTop = strip.back();
    CHECK(tipBottom.position.x == Catch::Approx(1.0f));
    CHECK(tipBottom.position.y == Catch::Approx(-0.5f));
    CHECK(tipTop.position.y == Catch::Approx(1.0f));
    // Textured by age: the tip at the cell's start, older edges further on.
    CHECK(tipTop.uv.x == Catch::Approx(0.0f));
    CHECK(tipTop.uv.y == Catch::Approx(1.0f));
    CHECK(tipBottom.uv.y == Catch::Approx(0.0f));
    CHECK(strip.front().uv.x > 0.0f);
}

TEST_CASE("edges age out after the lifetime", "[ribbon]") {
    Emitter r;
    r.init(4.0f, 0.5f, 1, 1);
    r.setVisible(true);
    r.setTransform(at({0, 0, 0}));
    r.update(0.0f, false);
    for (int i = 1; i <= 4; ++i) {
        r.setTransform(at({static_cast<float>(i), 0, 0}));
        r.update(0.25f, false);
    }
    CHECK(r.edgeCount() <= 2u);
    // Hidden, nothing new; what is there plays out.
    for (int i = 0; i < 4; ++i) r.update(0.25f, true);
    CHECK(r.edgeCount() == 0u);
}

TEST_CASE("a ribbon's colour and alpha are bytes, the alpha kept apart", "[ribbon]") {
    Emitter r;
    r.init(10.0f, 1.0f, 1, 1);
    r.setAlpha(0.5f);
    r.setColor({1.0f, 0.0f, 0.5f});
    CHECK(r.color() == 0x80FF0080u);
}

TEST_CASE("gravity pulls an edge by its age squared", "[ribbon]") {
    Emitter r;
    r.init(1.0f, 10.0f, 1, 1);
    r.setGravity(-2.0f);
    r.setVisible(true);
    r.setTransform(at({0, 0, 0}));
    r.update(0.0f, false);  // one edge at the origin, one second old
    r.setTransform(at({0, 0, 0}));
    r.update(1.0f, false);
    std::vector<Vertex> strip;
    r.appendStrip(strip);
    REQUIRE(strip.size() >= 4);
    // The oldest edge was laid down a whole step back, its age starting at
    // -1.0 and now 1.0001: gravity moves it by g x (1.0001^2 - 1.0^2) in all
    // (0x00980090 adds (2 x age + dt) x g x dt each step).
    CHECK(strip.front().position.z == Catch::Approx(-2.0f * (1.0001f * 1.0001f - 1.0f)).margin(2e-4));
}
