// The colour of a fire's light after its flames (rendering/rt_fire_color.hpp), with the
// particles of 3.3.5's own models.
#include <catch_amalgamated.hpp>

#include "rendering/rt_fire_color.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

using wowee::pipeline::M2ParticleEmitter;
using namespace wowee::rendering::rt_lights;

namespace {
struct Emitter {
    int blend;
    uint32_t flags;
    float rate, life;
    glm::vec3 texel;  // averageColor
    float coverage;   // alphaCoverage
    float middle;
    std::vector<glm::vec3> colors;
    std::vector<float> alphas, sizes;
};

std::optional<glm::vec3> flamesOf(const std::vector<Emitter>& emitters) {
    FlameColor flames;
    for (const Emitter& in : emitters) {
        M2ParticleEmitter e{};
        e.blendingType = static_cast<uint8_t>(in.blend);
        e.flags = in.flags;
        const std::vector<float> times = {0.0f, in.middle, 1.0f};
        e.particleColor.timestamps = e.particleAlpha.timestamps = e.particleScale.timestamps = times;
        e.particleColor.vec3Values = in.colors;
        e.particleAlpha.floatValues = in.alphas;
        e.particleScale.floatValues = in.sizes;
        flames.addEmitter(e, in.rate, in.life, in.texel * in.coverage);
    }
    return flames.color();
}

void near(const glm::vec3& c, float r, float g, float b) {
    INFO(c.r << " " << c.g << " " << c.b);
    CHECK(c.r == Catch::Approx(r).margin(0.02));
    CHECK(c.g == Catch::Approx(g).margin(0.02));
    CHECK(c.b == Catch::Approx(b).margin(0.02));
}

const glm::vec3 kFlameLick{0.877176f, 0.601376f, 0.323849f};  // flamelicksmall.blp
constexpr float kFlameLickCoverage = 0.0793457f;
}  // namespace

TEST_CASE("ordinary fires keep their colour", "[rt][fire]") {
    // FREESTANDINGTORCH01
    const auto torch = flamesOf({{4, 0x20001, 20.0f, 0.875f, kFlameLick, kFlameLickCoverage, 0.5f,
                                  {{0.917647f, 0.501961f, 0.0941177f}, glm::vec3(1.0f), glm::vec3(1.0f)},
                                  {0.588244f, 0.882351f, 0.0f}, {0.25f, 0.138889f, 0.00277778f}},
                                 {4, 0x20001, 10.0f, 0.82f, kFlameLick, kFlameLickCoverage, 0.6f,
                                  {{0.92549f, 0.411765f, 0.0705882f}, {0.890196f, 0.501961f, 0.290196f}, glm::vec3(1.0f)},
                                  {1.0f, 0.521561f, 0.0901822f}, {0.0555556f, 0.075f, 0.0416667f}}});
    REQUIRE(torch);
    near(*torch, 1.0f, 0.53f, 0.21f);
    CHECK(fireColor(true, torch) == kTorchColor);
    // ORCBRAZIER_CAMPFIRE01: the flames, not the smoke over them.
    const glm::vec3 smoke{0.420418f, 0.408648f, 0.398053f};
    const auto campfire = flamesOf({{2, 0x20029, 8.0f, 3.48f, smoke, 0.176025f, 0.5f,
                                     {{0.862745f, 0.466667f, 0.0431373f}, {0.760784f, 0.490196f, 0.27451f},
                                      {0.25098f, 0.054902f, 0.054902f}},
                                     {0.537248f, 0.329417f, 0.0f}, {0.277778f, 0.516667f, 0.752778f}},
                                    {4, 0x20009, 35.0f, 1.465f, kFlameLick, kFlameLickCoverage, 0.7f,
                                     {{0.964706f, 0.403922f, 0.0980392f}, glm::vec3(1.0f), glm::vec3(1.0f)},
                                     {1.0f, 1.0f, 0.109806f}, {0.252778f, 0.427778f, 0.108333f}}});
    REQUIRE(campfire);
    near(*campfire, 1.0f, 0.55f, 0.26f);
    CHECK(fireColor(false, campfire) == kFireColor);
    // STORMWINDDWARFBRAZIER, with a smoke as bright
    const auto dwarf = flamesOf({{2, 0x20009, 6.0f, 3.77f, smoke, 0.176025f, 0.5f,
                                  {{0.8f, 0.521569f, 0.27451f}, {0.764706f, 0.717647f, 0.670588f}, glm::vec3(1.0f)},
                                  {0.458815f, 0.772546f, 0.0f}, {0.555556f, 0.794444f, 1.16389f}},
                                 {4, 0x20009, 3.5f, 2.23f, {0.163733f, 0.0616958f, 0.0303692f}, 1.0f, 0.5f,
                                  {{1.0f, 0.635294f, 0.0f}, glm::vec3(1.0f), glm::vec3(1.0f)},
                                  {1.0f, 0.725486f, 0.0f}, {0.0666667f, 0.0416667f, 0.00555556f}}});
    REQUIRE(dwarf);
    near(*dwarf, 1.0f, 0.30f, 0.09f);
    CHECK(fireColor(false, dwarf) == kFireColor);
    // STORMWINDBRAZIER01's and ORCBONFIRE's, redder than the light.
    CHECK(fireColor(false, glm::vec3(1.0f, 0.28f, 0.04f)) == kFireColor);
    CHECK(fireColor(false, glm::vec3(1.0f, 0.32f, 0.05f)) == kFireColor);
    CHECK(fireColor(false, std::nullopt) == kFireColor);
    CHECK(fireColor(true, std::nullopt) == kTorchColor);
}

TEST_CASE("other flames give their own", "[rt][fire]") {
    // SCHOLOMANCEBRAZIER01GREEN
    const auto green = flamesOf({{4, 0x20029, 8.0f, 3.5f, {0.899901f, 0.665914f, 0.986849f}, 0.0966797f, 0.35f,
                                  {{0.933333f, 0.811765f, 0.384314f}, {0.384314f, 0.682353f, 0.0941177f},
                                   {0.0f, 0.392157f, 0.192157f}},
                                  {0.0f, 1.0f, 0.0f}, {0.277778f, 0.416667f, 0.555556f}},
                                 {4, 0x20229, 2.0f, 1.45f, {0.334788f, 0.204546f, 0.0730909f}, 1.0f, 0.4f,
                                  {{0.745098f, 0.839216f, 0.376471f}, {0.337255f, 0.658824f, 0.0745098f},
                                   {0.0f, 0.517647f, 0.121569f}},
                                  {0.0f, 0.588244f, 0.0f}, {0.222222f, 0.416667f, 0.555556f}},
                                 {4, 0x20029, 8.0f, 8.0f, {1.0f, 0.763614f, 0.314797f}, 0.189453f, 0.35f,
                                  {{0.866667f, 0.74902f, 0.439216f}, {0.541176f, 0.752941f, 0.0313726f},
                                   {0.0f, 0.372549f, 0.105882f}},
                                  {0.0f, 1.0f, 0.0f}, {0.0277778f, 0.0138889f, 2.77778e-05f}}});
    REQUIRE(green);
    near(*green, 0.71f, 1.0f, 0.30f);
    CHECK(fireColor(false, green) == *green);
    // MEDIUMBRAZIERPURPLE01
    const auto violet = flamesOf({{4, 0x20029, 20.0f, 1.0f, {0.320618f, 0.455251f, 0.835222f}, 0.0795898f, 0.5f,
                                   {{1.0f, 0.0f, 0.917647f}, {0.960784f, 0.54902f, 0.996078f}, {0.992157f, 0.921569f, 1.0f}},
                                   {0.588244f, 1.0f, 0.0f}, {0.144472f, 0.522222f, 0.00277778f}}});
    REQUIRE(violet);
    near(*violet, 0.38f, 0.26f, 1.0f);
    CHECK(fireColor(false, violet) == *violet);
    // A lamp's glass the same (lampColor): a green lamp's light is green (T436).
    CHECK(lampColor(green) == *green);
    CHECK(lampColor(violet) == *violet);
    // ORCBONFIRE_BLUE, of the same texture as an orange one
    const auto blue = flamesOf({{4, 0x20029, 30.0f, 2.0f, kFlameLick, kFlameLickCoverage, 0.5f,
                                 {{0.0f, 0.0f, 1.0f}, {0.184314f, 0.737255f, 0.87451f}, {0.694118f, 0.917647f, 1.0f}},
                                 {0.588244f, 1.0f, 0.0f}, {2.77778e-05f, 1.38889f, 0.138889f}},
                                {4, 0x20039, 20.0f, 3.0f, {0.0524902f, 0.104289f, 0.140332f}, 1.0f, 0.5f,
                                 {{0.305882f, 0.0f, 0.866667f}, {0.713726f, 0.733333f, 1.0f}, {0.764706f, 0.992157f, 1.0f}},
                                 {0.196081f, 0.784326f, 0.0784326f}, {0.694444f, 0.277778f, 0.0138889f}}});
    REQUIRE(blue);
    near(*blue, 0.48f, 0.96f, 1.0f);
    CHECK(fireColor(true, blue) == *blue);
    // G_BRAZIERORCRED, red and no orange: mostly its own.
    const glm::vec3 red = fireColor(false, glm::vec3(1.0f, 0.21f, 0.27f));
    CHECK(red.r == 1.0f);
    CHECK(red.g < 0.35f);
    CHECK(red.b > 0.24f);
}

TEST_CASE("smoke counts where there is nothing else, nothing not at all", "[rt][fire]") {
    const Emitter smoke{2, 0x1, 8.0f, 3.0f, {0.5f, 0.5f, 1.0f}, 0.2f, 0.5f,
                        {glm::vec3(1.0f), glm::vec3(1.0f), glm::vec3(1.0f)}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}};
    const auto alone = flamesOf({smoke});
    REQUIRE(alone);
    near(*alone, 0.5f, 0.5f, 1.0f);
    Emitter none = smoke;
    none.rate = 0.0f;
    CHECK_FALSE(flamesOf({none}));
    none = smoke;
    none.flags = 0;  // lit by the world
    CHECK_FALSE(flamesOf({none}));
    CHECK_FALSE(flamesOf({}));
    // A card of the model where it has no particles.
    FlameColor cards;
    cards.added = glm::vec3(0.1f, 0.2f, 0.4f);
    REQUIRE(cards.color());
    near(*cards.color(), 0.25f, 0.5f, 1.0f);
}

TEST_CASE("a particle's value over its life", "[rt][fire]") {
    CHECK(overLife<float>({}, {}, 0.5f, 7.0f) == 7.0f);
    CHECK(overLife<float>({0.0f, 1.0f}, {3.0f}, 0.5f, 7.0f) == 3.0f);
    CHECK(overLife<float>({0.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 5.0f}, 0.25f, 7.0f) == 0.5f);
    CHECK(overLife<float>({0.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 5.0f}, 0.75f, 7.0f) == 3.0f);
    CHECK(overLife<float>({0.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 5.0f}, 2.0f, 7.0f) == 5.0f);
    CHECK(overLife<float>({0.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 5.0f}, -1.0f, 7.0f) == 0.0f);
}

namespace {
struct Spark {
    int emitterIndex = 0;
    float life = 0.0f, maxLife = 1.0f, sizeVary = 1.0f;
    uint8_t twinkleSeed = 0;
};
// A fire's pool: n particles of the emitter, their ages and their places in the
// twinkle's table spread as a running emitter's are.
std::vector<Spark> pool(int emitter, int n) {
    std::vector<Spark> out;
    for (int i = 0; i < n; ++i)
        out.push_back({.emitterIndex = emitter, .life = (static_cast<float>(i) + 0.5f) / static_cast<float>(n),
                       .maxLife = 1.0f, .sizeVary = 1.0f, .twinkleSeed = static_cast<uint8_t>((i * 37) & 0x7f)});
    return out;
}
M2ParticleEmitter flame(int blend, float percent, float scaleMin, float scaleMax) {
    M2ParticleEmitter e{};
    e.blendingType = static_cast<uint8_t>(blend);
    e.particleScale.timestamps = {0.0f, 0.5f, 1.0f};
    e.particleScale.floatValues = {0.28f, 1.11f, 0.19f};
    e.particleAlpha.timestamps = {0.0f, 0.5f, 1.0f};
    e.particleAlpha.floatValues = {0.24f, 0.27f, 0.04f};
    e.twinkleSpeed = 0.1f;
    e.twinklePercent = percent;
    e.twinkleScaleMin = scaleMin;
    e.twinkleScaleMax = scaleMax;
    return e;
}
}  // namespace

// The user's answer to V417: a fire light is as strong as what of its flames is drawn.
TEST_CASE("a fire light is as much of a fire as its flames drawn", "[rt][fire]") {
    // Flames the twinkle leaves whole (1/1/1, nearly every fire): the whole light.
    CHECK(visibleFlameShare({flame(4, 1.0f, 1.0f, 1.0f)}, pool(0, 200)) == 1.0f);
    // UndeadFireLarge's 0..1: on the mean a third of the area.
    const float undead = visibleFlameShare({flame(4, 1.0f, 0.0f, 1.0f)}, pool(0, 230));
    CHECK(undead > 0.25f);
    CHECK(undead < 0.42f);
    // Half of them hidden (percent 0.5), the rest whole.
    const float half = visibleFlameShare({flame(4, 0.5f, 1.0f, 1.0f)}, pool(0, 230));
    CHECK(half > 0.38f);
    CHECK(half < 0.62f);
    // The smoke over the flames counts for nothing beside them.
    std::vector<Spark> both = pool(0, 200);
    for (Spark s : pool(1, 200)) both.push_back(s);
    CHECK(visibleFlameShare({flame(4, 1.0f, 1.0f, 1.0f), flame(2, 0.0f, 1.0f, 1.0f)}, both) == 1.0f);
    // Nothing alive: emission decides whether it burns.
    CHECK(visibleFlameShare({flame(4, 1.0f, 0.0f, 1.0f)}, std::vector<Spark>{}) == 1.0f);
}

TEST_CASE("the fire light of a fire without a light of its own follows its flames", "[rt][fire]") {
    const std::string renderer = wowee::test::slurp("src/rendering/m2_renderer.cpp");
    REQUIRE_FALSE(renderer.empty());
    const size_t fire = renderer.find("// A fire with no light of its own (rt_lights::fireLight), while");
    REQUIRE(fire != std::string::npos);
    const size_t pushed = renderer.find("pushAlongParticles(l, *model, inst, out);", fire);
    REQUIRE(pushed != std::string::npos);
    CHECK(renderer.substr(fire, pushed - fire).find("visibleFlameShare(model->particleEmitters, inst.particles)") !=
          std::string::npos);
}

TEST_CASE("A lamp's light is its glass's colour only where that is clearly another", "[rt_lights][lamp]") {
    // No glass known, or pale glass (its mean says little of the light): the warm own.
    CHECK(lampColor(std::nullopt) == kLampColor);
    CHECK(lampColor(glm::vec3(1.0f, 1.0f, 1.0f)) == kLampColor);
    CHECK(lampColor(glm::vec3(1.0f, 0.95f, 0.85f)) == kLampColor);
    CHECK(lampColor(glm::vec3(0.0f)) == kLampColor);
    // Warm yellow or orange glass, as a human street lamp's: the own.
    CHECK(lampColor(glm::vec3(1.0f, 0.8f, 0.45f)) == kLampColor);
    CHECK(lampColor(glm::vec3(1.0f, 0.7f, 0.4f)) == kLampColor);
    // Green, blue: theirs, brightest channel 1.
    const glm::vec3 green = lampColor(glm::vec3(0.3f, 1.0f, 0.35f));
    CHECK(green.g == Catch::Approx(1.0f));
    CHECK(green.r < 0.5f);
    CHECK(green.b < 0.5f);
    const glm::vec3 blue = lampColor(glm::vec3(0.4f, 0.6f, 1.0f));
    CHECK(blue.b == Catch::Approx(1.0f));
    CHECK(blue.r < 0.6f);
    // Never brighter than one, never negative.
    for (const glm::vec3& g : {glm::vec3(5.0f, 0.1f, 0.1f), glm::vec3(0.1f, 3.0f, 0.2f)}) {
        const glm::vec3 c = lampColor(g);
        CHECK(std::max(c.r, std::max(c.g, c.b)) == Catch::Approx(1.0f));
        CHECK(std::min(c.r, std::min(c.g, c.b)) >= 0.0f);
    }
}
