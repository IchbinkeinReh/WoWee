// The light of what glows (rt_glow.hpp): a model's glowing triangles gathered
// into a few patches at load, each patch's light, an emitter's particles
// summed into one light, and how much of a lamp's or a glow's light is left
// by day. None of it fails loudly: a patch in the wrong place lights the wrong
// wall, a light that grows without bound washes a city out.
#include <catch_amalgamated.hpp>

#include "rendering/rt_glow.hpp"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::rendering::rt_lights;

namespace {

/// A square of side `side` at `at`, lying flat, as two triangles.
void addSquare(GlowBuilder& b, const glm::vec3& at, float side, const glm::vec3& color, uint32_t batch = 0,
               bool animated = false) {
    const glm::vec3 x(side, 0.0f, 0.0f), y(0.0f, side, 0.0f);
    b.addTriangle(at, at + x, at + x + y, color, batch, animated);
    b.addTriangle(at, at + x + y, at + y, color, batch, animated);
}

float brightest(const glm::vec3& c) { return std::max(c.r, std::max(c.g, c.b)); }

}  // namespace

TEST_CASE("A lamp's glass is one patch at its middle, of its colour", "[rt_glow]") {
    GlowBuilder b;
    addSquare(b, glm::vec3(1.0f, 1.0f, 3.0f), 0.5f, glm::vec3(1.0f, 0.8f, 0.4f));
    const auto patches = b.finish();
    REQUIRE(patches.size() == 1);
    CHECK(patches[0].centre.x == Catch::Approx(1.25f));
    CHECK(patches[0].centre.y == Catch::Approx(1.25f));
    CHECK(patches[0].centre.z == Catch::Approx(3.0f));
    CHECK(patches[0].area == Catch::Approx(0.25f));
    CHECK(patches[0].color.g == Catch::Approx(0.8f));
    CHECK(patches[0].animatedBatch == ~0u);
}

TEST_CASE("A long glow becomes a few patches and keeps all its area", "[rt_glow]") {
    GlowBuilder b;
    float total = 0.0f;
    // Twenty squares along a lava flow, sixty yards long.
    for (int i = 0; i < 20; ++i) {
        addSquare(b, glm::vec3(3.0f * float(i), 0.0f, 0.0f), 2.0f, glm::vec3(1.0f, 0.4f, 0.0f));
        total += 4.0f;
    }
    const auto patches = b.finish();
    REQUIRE(patches.size() == kMaxGlowPatches);
    float area = 0.0f;
    for (const auto& p : patches) area += p.area;
    CHECK(area == Catch::Approx(total));
    // Spread along it, not heaped at one end.
    float lo = 1e9f, hi = -1e9f;
    for (const auto& p : patches) {
        lo = std::min(lo, p.centre.x);
        hi = std::max(hi, p.centre.x);
    }
    CHECK(hi - lo > 20.0f);
}

TEST_CASE("What glows too dimly or too little makes no patch", "[rt_glow]") {
    GlowBuilder dim;
    addSquare(dim, glm::vec3(0.0f), 1.0f, glm::vec3(0.05f));
    CHECK(dim.finish().empty());
    GlowBuilder tiny;
    addSquare(tiny, glm::vec3(0.0f), 0.01f, glm::vec3(1.0f));
    CHECK(tiny.finish().empty());
    GlowBuilder none;
    CHECK(none.empty());
    CHECK(none.finish().empty());
}

TEST_CASE("A patch names the batch whose colour moves it", "[rt_glow]") {
    GlowBuilder b;
    addSquare(b, glm::vec3(0.0f), 0.3f, glm::vec3(1.0f), 2, false);
    addSquare(b, glm::vec3(0.5f, 0.0f, 0.0f), 0.4f, glm::vec3(1.0f), 5, true);
    const auto patches = b.finish();
    REQUIRE(patches.size() == 1);
    CHECK(patches[0].animatedBatch == 5u);
}

TEST_CASE("A glow's light grows with it and stays bounded", "[rt_glow]") {
    const glm::vec3 warm(1.0f, 0.6f, 0.2f);
    const PointLight candle = glowLight(glm::vec3(0.0f), warm, 0.01f, Source::Emissive);
    const PointLight lamp = glowLight(glm::vec3(0.0f), warm, 0.3f, Source::Emissive);
    const PointLight lava = glowLight(glm::vec3(0.0f), warm, 400.0f, Source::Emissive);
    CHECK(candle.source == Source::Emissive);
    CHECK(candle.attenuationEnd == Catch::Approx(kGlowMinReach));
    CHECK(lamp.attenuationEnd > candle.attenuationEnd);
    CHECK(lava.attenuationEnd == Catch::Approx(kGlowMaxReach));
    CHECK(brightest(lamp.color) >= brightest(candle.color));
    CHECK(brightest(lava.color) == Catch::Approx(kGlowMaxStrength));
    CHECK(brightest(candle.color) < 0.5f);
    CHECK(lamp.attenuationStart < lamp.attenuationEnd);
    // Its hue is the glow's.
    CHECK(lamp.color.r > lamp.color.g);
    CHECK(lamp.color.g > lamp.color.b);
    // Nothing for nothing, and nothing from a bad area.
    CHECK(brightest(glowLight(glm::vec3(0.0f), glm::vec3(0.0f), 1.0f, Source::Emissive).color) == 0.0f);
    CHECK(brightest(glowLight(glm::vec3(0.0f), warm, 0.0f, Source::Emissive).color) == 0.0f);
    CHECK(brightest(glowLight(glm::vec3(0.0f), warm, NAN, Source::Emissive).color) == 0.0f);
}

TEST_CASE("An emitter's light sits among its particles, of their colour", "[rt_glow]") {
    ParticleGlow g;
    CHECK_FALSE(g.light().has_value());
    g.add(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.5f, 0.0f), 1.0f, 0.1f, 1.0f);
    g.add(glm::vec3(2.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.5f, 0.0f), 1.0f, 0.1f, 1.0f);
    // An invisible one counts for nothing.
    g.add(glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 0.5f, 1.0f);
    CHECK(g.count == 2u);
    const auto l = g.light();
    REQUIRE(l.has_value());
    CHECK(l->source == Source::Particle);
    CHECK(l->position.x == Catch::Approx(1.0f));
    CHECK(l->position.z == Catch::Approx(1.0f));
    CHECK(l->color.b == Catch::Approx(0.0f));
    CHECK(l->color.r > l->color.g);

    // More of them, or bigger, light more and farther.
    ParticleGlow more = g;
    for (int i = 0; i < 20; ++i) more.add(glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.5f, 0.0f), 1.0f, 0.1f, 1.0f);
    CHECK(more.light()->attenuationEnd > l->attenuationEnd);
    CHECK(more.light()->attenuationEnd <= kParticleMaxReach);
    CHECK(brightest(more.light()->color) <= kParticleGain * kGlowMaxStrength + 1e-4f);
}

TEST_CASE("A model's emitters together are capped at a fire's strength", "[rt_glow]") {
    std::vector<PointLight> lights(3);
    lights[0].color = glm::vec3(5.0f);  // another model's, before the range
    for (size_t i = 1; i < 3; ++i) lights[i].color = glm::vec3(0.4f, 0.2f, 0.0f);
    capTotalStrength(lights, 1, 1.0f);
    CHECK(lights[0].color.r == 5.0f);  // untouched
    CHECK(lights[1].color.r == Catch::Approx(0.4f));  // under the cap: as it was
    for (int i = 0; i < 10; ++i) {
        PointLight l;
        l.color = glm::vec3(0.4f, 0.2f, 0.0f);
        lights.push_back(l);
    }
    capTotalStrength(lights, 1, 1.0f);
    float total = 0.0f;
    for (size_t i = 1; i < lights.size(); ++i) total += lights[i].color.r;
    CHECK(total == Catch::Approx(1.0f));
    CHECK(lights[1].color.g == Catch::Approx(lights[1].color.r * 0.5f));  // hue kept
}

TEST_CASE("The adds give their light, smoke and modulates none", "[rt_glow]") {
    CHECK(particleGlowWeight(4, false) == 1.0f);
    CHECK(particleGlowWeight(3, true) == 1.0f);
    CHECK(particleGlowWeight(2, true) > 0.0f);
    CHECK(particleGlowWeight(2, true) < particleGlowWeight(4, true));
    CHECK(particleGlowWeight(2, false) == 0.0f);
    CHECK(particleGlowWeight(0, false) == 0.0f);
    CHECK(particleGlowWeight(5, true) == 0.0f);
    CHECK(particleGlowWeight(6, true) == 0.0f);
}
