// Waves from a swimmer's limbs at the surface.

#include "rendering/swim_wave.hpp"

#include <catch_amalgamated.hpp>

using namespace wowee::rendering::swim_wave;

TEST_CASE("a limb moving at the surface starts a ring", "[swim_wave]") {
    std::vector<Wave> waves;
    Limb limb;
    limbMoved(waves, limb, glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 0.1f);
    REQUIRE(waves.empty());  // nothing to compare with yet
    limbMoved(waves, limb, glm::vec3(0.25f, 0.0f, 10.0f), 10.0f, 0.1f);
    REQUIRE(waves.size() == 1);
    REQUIRE(waves[0].strength == Catch::Approx(0.5f));
}

TEST_CASE("a still limb or one away from the surface starts none", "[swim_wave]") {
    std::vector<Wave> waves;
    Limb limb;
    limbMoved(waves, limb, glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 0.1f);
    limbMoved(waves, limb, glm::vec3(0.01f, 0.0f, 10.0f), 10.0f, 0.1f);
    REQUIRE(waves.empty());
    limbMoved(waves, limb, glm::vec3(1.0f, 0.0f, 11.0f), 10.0f, 0.1f);
    REQUIRE(waves.empty());
}

TEST_CASE("one limb waits between rings, and the oldest ring gives way", "[swim_wave]") {
    std::vector<Wave> waves;
    Limb limb;
    float x = 0.0f;
    limbMoved(waves, limb, glm::vec3(x, 0.0f, 10.0f), 10.0f, 0.05f);
    limbMoved(waves, limb, glm::vec3(x += 0.2f, 0.0f, 10.0f), 10.0f, 0.05f);
    limbMoved(waves, limb, glm::vec3(x += 0.2f, 0.0f, 10.0f), 10.0f, 0.05f);
    REQUIRE(waves.size() == 1);
    for (int i = 0; i < 200; ++i) limbMoved(waves, limb, glm::vec3(x += 0.2f, 0.0f, 10.0f), 10.0f, 0.2f);
    REQUIRE(waves.size() == kMaxWaves);
}

TEST_CASE("another unit's limb makes no ring once the rings are full", "[swim_wave]") {
    std::vector<Wave> waves(kMaxWaves);
    waves.front().strength = 0.5f;
    Limb limb;
    limbMoved(waves, limb, glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 0.1f, false);
    limbMoved(waves, limb, glm::vec3(1.0f, 0.0f, 10.0f), 10.0f, 0.1f, false);
    REQUIRE(waves.size() == kMaxWaves);
    REQUIRE(waves.front().strength == 0.5f);
    waves.pop_back();
    limbMoved(waves, limb, glm::vec3(2.0f, 0.0f, 10.0f), 10.0f, 0.1f, false);
    REQUIRE(waves.size() == kMaxWaves);
    REQUIRE(waves.back().position == glm::vec2(2.0f, 0.0f));
}

TEST_CASE("a ring ends", "[swim_wave]") {
    std::vector<Wave> waves(1);
    age(waves, kLife * 0.5f);
    REQUIRE(waves.size() == 1);
    age(waves, kLife);
    REQUIRE(waves.empty());
}
