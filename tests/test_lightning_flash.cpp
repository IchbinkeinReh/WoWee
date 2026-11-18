// A thunderstorm's lightning lighting up the world (rendering/lightning_flash.hpp):
// the strike's shape over time, when the weather strikes, what a flash does to
// the light's colours, and that the lighting and the ray traced probes take it
// as they should.
#include <catch_amalgamated.hpp>

#include "rendering/lightning_flash.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace lf = wowee::rendering::lightning_flash;

namespace {

std::string readSource(const char* rel) {
    std::ifstream f(std::string(WOWEE_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

float peakBetween(const lf::Strike& s, float a, float b) {
    float m = 0.0f;
    for (float t = a; t <= b; t += 0.0005f) m = std::max(m, lf::envelope(s, t));
    return m;
}

}  // namespace

TEST_CASE("a strike flashes at once, flickers with its strokes and is gone within its length", "[lightning]") {
    const lf::Strike s = lf::referenceStrike();
    CHECK(lf::envelope(s, -0.01f) == 0.0f);
    // The first stroke reaches full brightness in its rise.
    CHECK(lf::envelope(s, lf::kStrokeRise) == Catch::Approx(1.0f));
    // Between strokes it falls back, then rises again with the next one.
    const float dip = lf::envelope(s, s.strokes[1].at - 0.001f);
    CHECK(dip < 0.4f);
    CHECK(peakBetween(s, s.strokes[1].at, s.strokes[1].at + 0.03f) > dip + 0.2f);
    CHECK(peakBetween(s, s.strokes[2].at, s.strokes[2].at + 0.03f) == Catch::Approx(0.85f).margin(0.02f));
    // Half a second on only the afterglow is left, and at the end nothing.
    CHECK(lf::envelope(s, 0.5f) < 0.15f);
    CHECK(lf::envelope(s, lf::kStrikeLength) == 0.0f);
    CHECK(lf::envelope(s, lf::kStrikeLength + 1.0f) == 0.0f);
    for (float t = 0.0f; t < 1.5f; t += 0.001f) {
        const float v = lf::envelope(s, t);
        CHECK((v >= 0.0f && v <= 1.0f));
    }
}

TEST_CASE("a far strike is dimmer than a near one", "[lightning]") {
    lf::Strike s = lf::referenceStrike();
    s.strength = 0.5f;
    CHECK(peakBetween(s, 0.0f, 1.0f) == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("random strikes keep to their shape", "[lightning]") {
    std::mt19937 rng(7);
    for (int i = 0; i < 200; ++i) {
        const lf::Strike s = lf::randomStrike(rng);
        REQUIRE(s.count >= 1);
        REQUIRE(s.count <= lf::kMaxStrokes);
        CHECK(s.strokes[0].at == 0.0f);
        CHECK(s.strokes[0].amplitude == 1.0f);
        for (int k = 1; k < s.count; ++k) {
            CHECK(s.strokes[k].at > s.strokes[k - 1].at);
            CHECK(s.strokes[k].at - s.strokes[k - 1].at <= 0.15f + 1e-4f);
        }
        CHECK(s.strength >= 0.45f);
        CHECK(s.strength <= 1.0f);
        // Over well before the strike's length.
        CHECK(lf::envelope(s, lf::kStrikeLength - 0.01f) < 0.01f);
    }
}

TEST_CASE("only the thunderstorm strikes", "[lightning]") {
    CHECK(lf::strikes(lf::kThunderstormWeather, 1.0f));
    CHECK_FALSE(lf::strikes(lf::kThunderstormWeather, 0.0f));
    for (uint32_t row : {0u, 1u, 3u, 4u, 5u, 21u, 22u, 99u})  // clear, rain, snow, sand
        CHECK_FALSE(lf::strikes(row, 1.0f));

    lf::Scheduler sched;
    for (int i = 0; i < 60 * 120; ++i) {
        sched.update(1.0f / 60.0f, 5u, 1.0f);  // heavy rain, two minutes
        REQUIRE(sched.value() == 0.0f);
    }
}

TEST_CASE("a storm strikes every few seconds, more often the heavier it is", "[lightning]") {
    const auto count = [](float intensity) {
        lf::Scheduler sched(11);
        int strikes = 0;
        bool was = false;
        for (int i = 0; i < 60 * 600; ++i) {  // ten minutes at 60 fps
            sched.update(1.0f / 60.0f, lf::kThunderstormWeather, intensity);
            const bool now = sched.active() && sched.sinceStrike() < 0.02f;
            if (now && !was) ++strikes;
            was = now;
        }
        return strikes;
    };
    const int full = count(1.0f);
    const int light = count(0.2f);
    CHECK(full > 50);   // about every 8 s
    CHECK(full < 110);
    CHECK(light > 15);
    CHECK(light < full);
}

TEST_CASE("the flash goes by the time since the strike, not by the frames", "[lightning]") {
    // Two frame rates through the same strike see the same light at the same time.
    lf::Scheduler a, b;
    a.start(lf::referenceStrike());
    b.start(lf::referenceStrike());
    for (int i = 0; i < 12; ++i) a.update(1.0f / 120.0f, 0u, 0.0f);
    for (int i = 0; i < 3; ++i) b.update(1.0f / 30.0f, 0u, 0.0f);
    CHECK(a.value() == Catch::Approx(b.value()).margin(1e-4f));
    CHECK(a.value() == Catch::Approx(lf::envelope(lf::referenceStrike(), 0.1f)).margin(1e-4f));
}

TEST_CASE("a pinned strike holds still until let go", "[lightning]") {
    lf::Scheduler sched;
    sched.pin(lf::referenceStrike(), 0.16f + lf::kStrokeRise);
    const float held = sched.value();
    for (int i = 0; i < 100; ++i) sched.update(0.05f, lf::kThunderstormWeather, 1.0f);
    CHECK(sched.value() == held);
    CHECK(held == Catch::Approx(0.85f).margin(0.01f));
    sched.pin(lf::referenceStrike(), -1.0f);
    CHECK(sched.value() == 0.0f);
}

TEST_CASE("a flash only ever brightens, toward a cold white", "[lightning]") {
    CHECK(lf::ambientAdd(0.0f) == glm::vec3(0.0f));
    CHECK(lf::ambientAdd(1.0f).b > lf::ambientAdd(1.0f).r);
    const glm::vec3 night(0.02f, 0.03f, 0.06f), day(0.55f, 0.6f, 0.65f), bright(1.0f, 0.95f, 0.9f);
    for (const glm::vec3& c : {night, day, bright}) {
        for (float f : {0.0f, 0.25f, 1.0f}) {
            for (const glm::vec3& out : {lf::skyBand(c, f), lf::cloud(c, f), lf::fog(c, f)}) {
                CHECK(out.r >= c.r);
                CHECK(out.g >= c.g);
                CHECK(out.b >= c.b);
            }
        }
        CHECK(lf::skyBand(c, 0.0f) == c);
        CHECK(lf::fog(c, 0.0f) == c);
    }
    // At night the sky lights up a great deal.
    CHECK(lf::skyBand(night, 1.0f).b > 0.5f);
    CHECK(lf::fog(night, 1.0f).b > 0.3f);
}

TEST_CASE("the lighting takes the flash, and the probes' open sky leaves it out", "[lightning]") {
    const std::string lm = readSource("src/rendering/lighting_manager.cpp");
    CHECK(lm.find("lf::ambientAdd(flash)") != std::string::npos);
    CHECK(lm.find("newParams.lightningAmbient = add") != std::string::npos);
    const std::string r = readSource("src/rendering/renderer.cpp");
    CHECK(r.find("lightingManager->setLightningFlash(flash)") != std::string::npos);
    CHECK(r.find("in.skyColor = glm::max(glm::vec3(currentFrameData.ambientColor) - lightningAmbientFrame_") !=
          std::string::npos);
    // The flash is set before the lighting it lights is worked out.
    const auto set = r.find("lightingManager->setLightningFlash(flash)");
    const auto upd = r.find("lightingManager->update(characterPosition");
    REQUIRE(set != std::string::npos);
    REQUIRE(upd != std::string::npos);
    CHECK(set < upd);
}
