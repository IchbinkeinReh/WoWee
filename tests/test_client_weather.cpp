// The original client's weather, as MapWeather.cpp makes it (Wow.exe 3.3.5a,
// build 12340): the density setting, how intensity becomes particles, where a
// drop, flake or grain starts and when it lands, the ground it lands on, and
// the player's movement the box leans with.
//
// The weather used to be 2000 points at most around the camera falling along
// the renderer's y axis - sideways - at a speed it made up, with zone weather
// it also made up when the server sent none.
#include <catch_amalgamated.hpp>

#include "rendering/client_weather.hpp"

#include <cmath>
#include <vector>

namespace cw = wowee::rendering::client_weather;

namespace {

// A fixed sequence, so a spawn can be followed by hand.
struct Sequence {
    std::vector<float> values;
    size_t next = 0;
    float operator()() { return values[next++ % values.size()]; }
};

cw::HeightCache flatGround(float height, int* asked = nullptr) {
    cw::HeightCache c;
    c.setQuery([height, asked](float, float, float) {
        if (asked) ++*asked;
        return height;
    });
    c.recenter({0.0f, 0.0f});
    return c;
}

}  // namespace

TEST_CASE("weatherDensity is a tenth, a third, two thirds or all", "[weather]") {
    CHECK(cw::densityScale(0) == Catch::Approx(0.1f));
    CHECK(cw::densityScale(1) == Catch::Approx(0.33f));
    CHECK(cw::densityScale(2) == Catch::Approx(0.66f));
    CHECK(cw::densityScale(3) == Catch::Approx(1.0f));
    CHECK(cw::kDefaultDensityLevel == 2);
}

TEST_CASE("the effects start at a quarter of the intensity", "[weather]") {
    CHECK(cw::effectStrength(0.0f) == 0.0f);
    CHECK(cw::effectStrength(0.25f) == 0.0f);
    CHECK(cw::effectStrength(1.0f) == Catch::Approx(1.0f));
    CHECK(cw::effectStrength(0.625f) == Catch::Approx(0.5f));
}

TEST_CASE("intensity eases over ten seconds for the whole range", "[weather]") {
    CHECK(cw::easedIntensity(0.0f, 1.0f, 0.0f) == 0.0f);
    CHECK(cw::easedIntensity(0.0f, 1.0f, 5.0f) == Catch::Approx(0.5f).margin(0.01f));
    CHECK(cw::easedIntensity(0.0f, 1.0f, 11.0f) == 1.0f);
    // Half the way is half the time.
    CHECK(cw::easedIntensity(0.2f, 0.7f, 5.1f) == Catch::Approx(0.7f));
}

TEST_CASE("rates, boxes and packets are the client's", "[weather]") {
    CHECK(cw::spawnRate(cw::Effect::Rain, 0.66f, 1.0f) == Catch::Approx(23100.0f));
    CHECK(cw::spawnRate(cw::Effect::Snow, 1.0f, 0.5f) == Catch::Approx(7000.0f));
    CHECK(cw::spawnRate(cw::Effect::Sand, 1.0f, 1.0f) == Catch::Approx(32000.0f));
    CHECK(cw::boxSize(cw::Effect::Rain) == glm::vec3(130.0f, 130.0f, 75.0f));
    CHECK(cw::boxSize(cw::Effect::Snow) == glm::vec3(90.0f, 90.0f, 60.0f));
    CHECK(cw::boxSize(cw::Effect::Sand) == glm::vec3(40.0f, 40.0f, 25.0f));
    CHECK(cw::kPacketSize == 6144u);
    // A frame counts for a sixtieth at most, and under one makes nothing.
    CHECK(cw::spawnCount(1.0f / 60.0f, 23100.0f) == 385);
    CHECK(cw::spawnCount(0.1f, 23100.0f) == 385);
    CHECK(cw::spawnCount(1.0f / 60.0f, 50.0f) == 0);
}

TEST_CASE("the client's own rotations", "[weather]") {
    // 0x004c3290: (1, 0) turned by 90 degrees goes to (0, -1).
    const glm::vec3 t = cw::yawRotation(1.5707964f).apply({1.0f, 0.0f, 0.0f});
    CHECK(t.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(t.y == Catch::Approx(-1.0f));
    // Standing still leans nothing.
    const glm::vec3 up = cw::leanForWind({0.3f, 0.0f}).apply({0.0f, 0.0f, 10.0f});
    CHECK(up.z == Catch::Approx(10.0f));
    // Running along +x at 18 yards a second leans the top of the box 65
    // degrees toward where the player is going.
    const glm::vec3 lean = cw::leanForWind({0.0f, 18.0f}).apply({0.0f, 0.0f, 1.0f});
    CHECK(std::acos(lean.z) == Catch::Approx(1.134464f).margin(1e-4));
    CHECK(lean.x > 0.8f);
    CHECK(std::abs(lean.y) < 1e-4f);
}

TEST_CASE("a rain drop starts at the top of the box and ends on the ground", "[weather]") {
    cw::HeightCache ground = flatGround(-10.0f);
    cw::SpawnContext ctx;
    ctx.strength = 1.0f;
    // Box centre, straight down: no spread, no horizontal speed variation.
    Sequence rnd{{0.5f, 0.5f, 0.5f, 0.5f, 0.5f}};
    const cw::Particle p = cw::spawnRain(std::ref(rnd), ctx, ground, 2.0f);
    // -28 - 4 - 2 * 0.5 yards a second down.
    CHECK(p.velocity.z == Catch::Approx(-33.0f));
    // 9.5 yards a second along -x (the angle is -1.57).
    CHECK(p.velocity.x == Catch::Approx(-9.5f).margin(0.01f));
    // Half the box's height above the camera, back along its path - near
    // enough: the start is worked back from the ground the trace found, which
    // is a cell's sample rather than a point on the path.
    const float half = 37.5f / 33.0f;
    CHECK(p.position.z == Catch::Approx(37.5f).margin(0.5f));
    CHECK(p.position.x == Catch::Approx(9.5f * half).margin(1.0f));
    CHECK(p.start == 2.0f);
    // It falls 47.5 yards to the ground.
    const float landing = p.position.z + p.velocity.z * (p.end - p.start);
    CHECK(landing == Catch::Approx(-10.0f).margin(0.6f));
    REQUIRE(cw::splashes(p, false));
    const cw::Splash s = cw::splashOf(p);
    CHECK(s.position.z == Catch::Approx(-10.0f).margin(0.6f));
    CHECK(s.end - s.start == Catch::Approx(0.25f));
    CHECK(cw::rainGone(p) == Catch::Approx(p.end + 2.0f / 33.0f));
}

TEST_CASE("a drop with no ground within reach is never drawn", "[weather]") {
    cw::HeightCache ground = flatGround(-500.0f);
    cw::SpawnContext ctx;
    ctx.strength = 1.0f;
    Sequence rnd{{0.5f}};
    const cw::Particle p = cw::spawnRain(std::ref(rnd), ctx, ground, 2.0f);
    CHECK(p.start == 0.0f);
    CHECK(p.end == 0.0f);
    CHECK_FALSE(cw::splashes(p, false));
}

TEST_CASE("riding, a drop ends at the box's floor without a splash", "[weather]") {
    cw::HeightCache ground = flatGround(-500.0f);
    cw::SpawnContext ctx;
    ctx.strength = 1.0f;
    ctx.riding = true;
    Sequence rnd{{0.5f}};
    const cw::Particle p = cw::spawnRain(std::ref(rnd), ctx, ground, 1.0f);
    const float landing = p.position.z + p.velocity.z * (p.end - p.start);
    CHECK(landing == Catch::Approx(-37.5f).margin(0.05f));
    CHECK_FALSE(cw::splashes(p, true));
}

TEST_CASE("a dead flake ends a quarter second before it starts", "[weather]") {
    cw::HeightCache ground = flatGround(100.0f);
    cw::SpawnContext ctx;
    ctx.strength = 0.5f;
    Sequence rnd{{0.5f}};
    const cw::Particle p = cw::spawnSnow(std::ref(rnd), ctx, ground, 3.0f);
    CHECK(p.start == 0.0f);
    CHECK(p.end == -0.25f);
}

TEST_CASE("snow falls slowly and lands", "[weather]") {
    cw::HeightCache ground = flatGround(-5.0f);
    cw::SpawnContext ctx;
    ctx.strength = 1.0f;
    Sequence rnd{{0.5f}};
    const cw::Particle p = cw::spawnSnow(std::ref(rnd), ctx, ground, 0.0f);
    CHECK(p.velocity.z == Catch::Approx(-2.0f - 3.5f - 0.5f));
    CHECK(p.position.z == Catch::Approx(30.0f).margin(0.5f));
    const float landing = p.position.z + p.velocity.z * (p.end - p.start);
    CHECK(landing == Catch::Approx(-5.0f).margin(0.6f));
    CHECK(cw::snowGone(p) == Catch::Approx(p.end + 0.25f));
}

TEST_CASE("sand blows in from the far side for about three seconds", "[weather]") {
    cw::HeightCache ground = flatGround(-100.0f);
    cw::SpawnContext ctx;
    Sequence rnd{{0.5f}};
    const cw::Particle p = cw::spawnSand(std::ref(rnd), ctx, ground, 1.0f);
    CHECK(p.position.x == Catch::Approx((0.5f * 0.15f + 0.85f) * 40.0f));
    CHECK(p.velocity.x == Catch::Approx(-18.666666f).margin(0.02f));
    CHECK(p.velocity.z == Catch::Approx(0.8333333f));
    CHECK(p.end - p.start == Catch::Approx(3.2f).margin(0.01f));
}

TEST_CASE("the ground is asked once a cell and only near the camera", "[weather]") {
    int asked = 0;
    cw::HeightCache ground = flatGround(0.0f, &asked);
    CHECK(ground.heightAt({1.0f, 1.0f, 5.0f}) == 0.0f);
    CHECK(ground.heightAt({0.5f, 0.9f, 9.0f}) == 0.0f);
    CHECK(asked == 1);
    // Two blocks either side of the camera's.
    CHECK(ground.heightAt({-60.0f, 0.0f, 0.0f}) == 0.0f);
    CHECK(ground.heightAt({-70.0f, 0.0f, 0.0f}) == std::numeric_limits<float>::max());
    CHECK(ground.heightAt({99.0f, 0.0f, 0.0f}) == 0.0f);
    CHECK(ground.heightAt({101.0f, 0.0f, 0.0f}) == std::numeric_limits<float>::max());
    // A step to the next block keeps what is still in the window.
    const size_t before = ground.cachedCells();
    ground.recenter({34.0f, 0.0f});
    CHECK(ground.cachedCells() == before - 1);
}

TEST_CASE("a trace stops at the first surface above it", "[weather]") {
    cw::HeightCache c;
    // A wall from x = 10 on.
    c.setQuery([](float x, float, float z) { return x >= 10.0f ? 50.0f : z - 200.0f; });
    c.recenter({0.0f, 0.0f});
    glm::vec3 hit;
    REQUIRE(c.trace({0.0f, 0.0f, 20.0f}, {20.0f, 0.0f, 0.0f}, hit));
    CHECK(hit.x >= 9.0f);
    CHECK(hit.x <= 11.0f);
    CHECK(hit.z == 50.0f);
    // Nothing in the way: false, and the end.
    REQUIRE_FALSE(c.trace({0.0f, 0.0f, 20.0f}, {5.0f, 0.0f, 0.0f}, hit));
    CHECK(hit == glm::vec3(5.0f, 0.0f, 0.0f));
}

TEST_CASE("the player's velocity is the last 150 ms of moves", "[weather]") {
    cw::VelocityWindow w;
    for (int i = 0; i < 20; ++i) w.add({0.07f, 0.0f, 0.0f}, 10);
    // The newest 15 moves make 150 ms: 1.05 yards over 0.151 seconds.
    CHECK(w.velocity().x == Catch::Approx(1.05f / 0.151f));
    const cw::Wind wind = cw::windFor(w.velocity(), 2.0f);
    CHECK(wind.angle == Catch::Approx(0.0f));
    CHECK(wind.speed == Catch::Approx(1.05f / 0.151f));
    // Slower than a yard a second: the way the player faces.
    CHECK(cw::windFor({0.5f, 0.0f, 0.0f}, 2.0f).angle == 2.0f);
    // Going -y wraps to the positive side.
    CHECK(cw::windFor({0.0f, -5.0f, 0.0f}, 0.0f).angle == Catch::Approx(4.712389f));
}

TEST_CASE("a row with no texture takes the effect's own", "[weather]") {
    CHECK(std::string(cw::defaultTexture(cw::Effect::Rain)) == "textures\\Weather\\RainDrop01.blp");
    CHECK(std::string(cw::defaultTexture(cw::Effect::Snow)) == "textures\\Weather\\SnowFlake01.blp");
    CHECK(std::string(cw::defaultTexture(cw::Effect::Sand)).empty());
}

TEST_CASE("mist is made past half strength for rain and snow", "[weather]") {
    CHECK(cw::mistRate(cw::Effect::Rain, 1.0f, 0.5f) == 0.0f);
    CHECK(cw::mistRate(cw::Effect::Rain, 1.0f, 1.0f) == Catch::Approx(38.0f));
    CHECK(cw::mistRate(cw::Effect::Snow, 0.66f, 1.0f) == Catch::Approx(48.0f * 0.66f));
    CHECK(cw::mistRate(cw::Effect::Sand, 1.0f, 0.5f) == Catch::Approx(32.0f));
}

TEST_CASE("a mist sheet sits on the ground and climbs over it", "[weather]") {
    cw::HeightCache ground = flatGround(-3.0f);
    cw::SpawnContext ctx;
    Sequence rnd{{0.5f}};
    cw::Mist m = cw::spawnMist(std::ref(rnd), cw::mistSpec(cw::Effect::Rain), ctx, ground, 10.0f);
    REQUIRE(m.live());
    // Six yards above the higher of the ground and where it was put, which
    // is a second and a half back along its path: half a yard down.
    CHECK(m.position.z == Catch::Approx(5.5f));
    CHECK(m.end - m.start == Catch::Approx(2.7f));
    // 5 yards a second for 2.7 seconds, a cell a step.
    CHECK(m.steps == static_cast<int>(std::lround(5.0f * 2.7f / 1.0416666f)));
    CHECK(m.velocity.z == Catch::Approx(0.33333334f));
    cw::stepMist(m, 10.0f, 10.1f);
    CHECK(m.position.z == Catch::Approx(5.5f + 0.033333f).margin(1e-4));
}

TEST_CASE("a mist sheet fades in, out, and near the camera", "[weather]") {
    cw::Mist m;
    m.start = 1.0f;
    m.end = 3.7f;
    CHECK(cw::mistAlpha(m, 1.0f, 50.0f) == 0.0f);
    CHECK(cw::mistAlpha(m, 1.2f, 50.0f) == Catch::Approx(0.5f));
    CHECK(cw::mistAlpha(m, 2.0f, 50.0f) == Catch::Approx(1.0f));
    CHECK(cw::mistAlpha(m, 3.5f, 50.0f) == Catch::Approx(0.5f));
    CHECK(cw::mistAlpha(m, 2.0f, 6.0f) == 0.0f);
    CHECK(cw::mistAlpha(m, 2.0f, 12.0f) == Catch::Approx(0.5f));
}

TEST_CASE("the weather's light is its intensity, held to 0.25, times the row's +0xc",
          "[weather]") {
    // 0x007846a0 / 0x00784850: out of clear weather the row's light applies
    // at once and the intensity eases in over ten seconds per 0.25.
    cw::WeatherLight w;
    CHECK(w.value(0.0) == 0.0f);
    w.set(cw::Effect::Rain, 0.8f, 0.6f, false, cw::Effect::None, 100.0);
    CHECK(w.intensityTo == Catch::Approx(0.25f));
    CHECK(w.value(100.0) == Catch::Approx(0.0f));
    CHECK(w.value(105.0) == Catch::Approx(0.125f * 0.6f).epsilon(1e-3));
    CHECK(w.value(200.0) == Catch::Approx(0.25f * 0.6f));
    // A new row eases its light over five seconds per 0.25 it moves.
    w.set(cw::Effect::Rain, 0.8f, 1.0f, false, cw::Effect::Rain, 200.0);
    CHECK(w.value(200.0) == Catch::Approx(0.25f * 0.6f));
    CHECK(w.value(204.0) == Catch::Approx(0.25f * 0.8f).epsilon(1e-3));
    CHECK(w.value(300.0) == Catch::Approx(0.25f));
    // Into clear weather the light stays the old row's; the intensity falls.
    w.set(cw::Effect::None, 0.0f, 1.0f, false, cw::Effect::Rain, 300.0);
    CHECK(w.lightTo == Catch::Approx(1.0f));
    CHECK(w.value(305.0) == Catch::Approx(0.125f).epsilon(2e-3));
    // Abrupt: at once.
    w.set(cw::Effect::Snow, 0.1f, 0.5f, true, cw::Effect::None, 400.0);
    CHECK(w.value(400.0) == Catch::Approx(0.05f));
}
