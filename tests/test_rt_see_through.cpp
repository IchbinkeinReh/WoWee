// The traced lights of a unit drawn see-through (rt_see_through.hpp): the few
// that give its body most, the torch in its own hand among them whatever
// stands behind it, what a building stops left out, and the same light of each
// as the trace gives a body (rt_lamp_light.glsli, run here itself).
#include <catch_amalgamated.hpp>

#include "rendering/rt_see_through.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace lamp_light {
using namespace glm;
#include "../assets/shaders/rt_lamp_light.glsli"
}  // namespace lamp_light

using namespace wowee::rendering;
using rt_lights::PointLight;

namespace {

PointLight lamp(glm::vec3 at, float strength, float end, rt_lights::Source source = rt_lights::Source::Model) {
    PointLight l;
    l.position = at;
    l.color = glm::vec3(strength, 0.5f * strength, 0.0f);
    l.attenuationEnd = end;
    l.source = source;
    return l;
}

std::string text(const char* path) {
    std::ifstream in(std::string(WOWEE_SOURCE_DIR) + path, std::ios::binary);
    REQUIRE(in);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("the shader's falloffs are the lights' own", "[rt][see_through]") {
    for (float start : {0.0f, 1.0f, 3.5f}) {
        for (float end : {4.0f, 15.0f, 30.0f}) {
            for (float d = 0.0f; d < 32.0f; d += 0.37f) {
                CHECK(lamp_light::rtLightFalloff(d, start, end) ==
                      Catch::Approx(rt_lights::attenuation(d, start, end)).margin(1e-5));
                CHECK(lamp_light::rtLightFalloffRealistic(d, start, end) ==
                      Catch::Approx(rt_lights::realisticAttenuation(d, start, end)).margin(1e-5));
            }
        }
    }
    // Straight on all of it, turned from it the wrap's, nothing past that.
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    CHECK(lamp_light::rtBodyLampLight(glm::vec3(0.0f, 0.0f, 1.0f), up, 2.0f, 8.0f, 0.25f, false) ==
          Catch::Approx(1.0f));
    CHECK(lamp_light::rtBodyLampLight(glm::vec3(1.0f, 0.0f, 0.0f), up, 2.0f, 8.0f, 0.25f, false) ==
          Catch::Approx(0.2f));
    CHECK(lamp_light::rtBodyLampLight(glm::vec3(0.0f, 0.0f, -1.0f), up, 2.0f, 8.0f, 0.25f, false) == 0.0f);
    CHECK(lamp_light::rtBodyLampLight(glm::vec3(0.0f, 0.0f, 9.0f), up, 2.0f, 8.0f, 0.25f, true) == 0.0f);
}

TEST_CASE("a body drawn see-through is lit by its own torch", "[rt][see_through]") {
    const glm::vec3 centre(100.0f, 50.0f, 11.0f);
    // The torch in its hand, a glow: from a yard over it. A lamp behind a
    // wall, another in the open, one whose reach ends short of it.
    const std::vector<PointLight> lights = {
        lamp(centre + glm::vec3(12.0f, 0.0f, 0.0f), 2.0f, 8.0f),
        lamp(centre + glm::vec3(0.0f, 5.0f, 0.0f), 1.0f, 15.0f),
        lamp(centre + glm::vec3(0.0f, -5.0f, 0.0f), 1.5f, 15.0f),
        lamp(centre + glm::vec3(0.7f, 0.0f, 0.2f), 0.6f, 6.0f, rt_lights::Source::Particle),
    };
    int asked = 0;
    const auto picked = rt_see_through::pick(lights, centre, 1.0f, false, [&](const glm::vec3& from) {
        ++asked;
        return from.y < centre.y ? 0.0f : 0.5f;
    });
    REQUIRE(picked.count == 2);
    // Only the two whose way to it may be barred are asked about.
    CHECK(asked == 2);
    CHECK(picked.lamps[0].position == lights[1].position);
    CHECK(picked.lamps[0].color.r == Catch::Approx(0.5f));
    CHECK(picked.lamps[0].end == 15.0f);
    CHECK(picked.lamps[1].position == lights[3].position + glm::vec3(0.0f, 0.0f, rt_see_through::kGlowLift));
    CHECK(picked.lamps[1].color.r == Catch::Approx(0.6f));

    const auto m = rt_see_through::pack(picked, 0.25f, true);
    CHECK(m[0][0] == glm::vec4(picked.lamps[0].position, 15.0f));
    CHECK(m[0][1] == glm::vec4(picked.lamps[0].color, 0.0f));
    CHECK(m[0][2] == glm::vec4(picked.lamps[1].position, 6.0f));
    CHECK(m[0][3] == glm::vec4(picked.lamps[1].color, 0.0f));
    CHECK(m[rt_see_through::kMatrices - 1][3] == glm::vec4(0.25f, 1.0f, 2.0f, 1.0f));
    CHECK(m[rt_see_through::kMatrices - 1][2] == glm::vec4(0.0f));
}

TEST_CASE("the strongest lights at a body drawn see-through", "[rt][see_through]") {
    std::vector<PointLight> lights;
    for (int i = 0; i < 20; ++i) {
        lights.push_back(lamp(glm::vec3(5.0f, 0.1f * static_cast<float>(i), 0.0f),
                              1.0f + static_cast<float>((i * 7) % 20), 40.0f));
    }
    const auto picked = rt_see_through::pick(lights, glm::vec3(0.0f), 1.0f, true, [](const glm::vec3&) { return 1.0f; });
    REQUIRE(picked.count == rt_see_through::kLights);
    for (int i = 0; i < picked.count; ++i) {
        CHECK(picked.lamps[static_cast<size_t>(i)].color.r == Catch::Approx(20.0f - static_cast<float>(i)));
    }
    // Full, the last column is still the settings'.
    const auto m = rt_see_through::pack(picked, 0.5f, false);
    CHECK(m[rt_see_through::kMatrices - 1][3] == glm::vec4(0.5f, 0.0f, 7.0f, 1.0f));
}

// What the test can tell of the pixels' side: the trace's result right behind
// a body is not what lights it once its own lights are there.
TEST_CASE("the shaders take a see-through body's own lights", "[rt][see_through]") {
    const std::string frag = text("/assets/shaders/character.frag.glsl");
    CHECK(frag.find("if (seeThrough && vSeeThroughLit > 0.5)") != std::string::npos);
    const std::string vert = text("/assets/shaders/character.vert.glsl");
    CHECK(vert.find("bones[MAX_BONES + 3u][3]") != std::string::npos);
    CHECK(rt_see_through::kMatrices == 4);
    const std::string trace = text("/assets/shaders/rt_trace.comp.glsl");
    CHECK(trace.find("const float RT_GLOW_LIFT = 1.0;") != std::string::npos);
    CHECK(trace.find("#include \"rt_lamp_light.glsli\"") != std::string::npos);
}
