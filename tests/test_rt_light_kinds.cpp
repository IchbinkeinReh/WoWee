// The Debug Graphics view of the lights' kinds (rt_light_kinds.hpp): a name
// and a colour for every kind of light, each colour its own, the trace
// shader's list the same as this one, and the GPU record carrying the kind.
#include <catch_amalgamated.hpp>

#include "rendering/rt_light_kinds.hpp"
#include "test_support.hpp"

#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace wowee::rendering;
namespace kinds = wowee::rendering::rt_light_kinds;

TEST_CASE("every kind of light has a name and a colour of its own", "[rt][lights][kinds]") {
    REQUIRE(kinds::kKinds.size() == rt_lights::kSourceCount);
    std::set<std::string> names;
    for (size_t i = 0; i < kinds::kKinds.size(); ++i) {
        const auto& k = kinds::kKinds[i];
        INFO(i);
        REQUIRE(k.name != nullptr);
        CHECK(std::strlen(k.name) > 2);
        names.insert(k.name);
        // Bright enough to read on a dark wall: one channel near full.
        CHECK(std::max(k.colour.r, std::max(k.colour.g, k.colour.b)) >= 0.95f);
        // Far from every other one.
        for (size_t j = 0; j < i; ++j) {
            INFO(j);
            CHECK(glm::length(k.colour - kinds::kKinds[j].colour) > 0.4f);
        }
    }
    CHECK(names.size() == kinds::kKinds.size());
}

TEST_CASE("the kinds are named in Source's order", "[rt][lights][kinds]") {
    CHECK(std::string(kinds::kindOf(rt_lights::Source::Fire).name) == "Fire");
    CHECK(std::string(kinds::kindOf(rt_lights::Source::Lamp).name) == "Lamp");
    CHECK(std::string(kinds::kindOf(rt_lights::Source::Particle).name) == "Particles");
    CHECK(std::string(kinds::kindOf(rt_lights::Source::Building).name).find("Building") == 0);
    CHECK(std::string(kinds::kindOf(rt_lights::Source::Unit).name).find("Unit") == 0);
    CHECK(std::string(kinds::kindOf(rt_lights::Source::Emissive).name).find("Glow") == 0);
    CHECK(std::string(kinds::kindOf(rt_lights::Source::Model).name).find("Model") == 0);
    // Out of range: the last, not past the end.
    CHECK(&kinds::kindOf(static_cast<rt_lights::Source>(200)) == &kinds::kKinds.back());
}

TEST_CASE("a light is drawn as its brightness in its kind's colour", "[rt][lights][kinds]") {
    // A white light of brightness one is the kind's colour itself.
    const glm::vec3 w = kinds::tinted(rt_lights::Source::Lamp, glm::vec3(1.0f));
    CHECK(w.r == Catch::Approx(kinds::kindOf(rt_lights::Source::Lamp).colour.r));
    CHECK(w.g == Catch::Approx(kinds::kindOf(rt_lights::Source::Lamp).colour.g));
    // A dimmer, coloured one scales it by its luminance; none gives none.
    const glm::vec3 c = kinds::tinted(rt_lights::Source::Fire, glm::vec3(0.5f, 0.0f, 0.0f));
    CHECK(c.r == Catch::Approx(0.5f * 0.2126f * kinds::kindOf(rt_lights::Source::Fire).colour.r));
    CHECK(kinds::tinted(rt_lights::Source::Unit, glm::vec3(0.0f)) == glm::vec3(0.0f));
}

TEST_CASE("the GPU record carries what gave the light", "[rt][lights][kinds]") {
    rt_lights::PointLight l;
    l.attenuationEnd = 5.0f;
    l.source = rt_lights::Source::Particle;
    CHECK(rt_lights::toGpu(l).storey.w == 6.0f);
    l.hasStorey = true;
    l.storeyFloor = 1.0f;
    l.storeyCeiling = 4.0f;
    l.source = rt_lights::Source::Building;
    CHECK(rt_lights::toGpu(l).storey == glm::vec4(1.0f, 4.0f, 1.0f, 3.0f));
}

TEST_CASE("the trace shader's colours are these", "[rt][lights][kinds]") {
    const std::string src = wowee::test::slurp("assets/shaders/rt_trace.comp.glsl");
    const size_t at = src.find("vec3 rtLightKindColour(int kind)");
    REQUIRE(at != std::string::npos);
    const size_t end = src.find("return kKinds", at);
    REQUIRE(end != std::string::npos);
    const std::string body = src.substr(at, end - at);
    const std::regex vec(R"(vec3\(([0-9.]+), ([0-9.]+), ([0-9.]+)\))");
    std::vector<glm::vec3> found;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), vec); it != std::sregex_iterator(); ++it) {
        found.emplace_back(std::stof((*it)[1]), std::stof((*it)[2]), std::stof((*it)[3]));
    }
    REQUIRE(found.size() == kinds::kKinds.size());
    for (size_t i = 0; i < found.size(); ++i) {
        INFO(i);
        CHECK(found[i].r == Catch::Approx(kinds::kKinds[i].colour.r));
        CHECK(found[i].g == Catch::Approx(kinds::kKinds[i].colour.g));
        CHECK(found[i].b == Catch::Approx(kinds::kKinds[i].colour.b));
    }
    // The view's number is the settings' entry for it.
    CHECK(src.find("const int RT_LIGHT_KIND_VIEW = " + std::to_string(kinds::kDebugView) + ";") !=
          std::string::npos);
}
