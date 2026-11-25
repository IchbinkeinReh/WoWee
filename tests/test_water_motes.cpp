// What floats in the water about the camera (0x0079e100, 0x0079bf40, 0x0079ca70).
#include <catch_amalgamated.hpp>

#include "rendering/water_motes.hpp"

namespace wm = wowee::rendering::water_motes;

TEST_CASE("the motes keep about the camera", "[water_motes]") {
    wm::Swarm swarm(7);
    const wm::Liquid still{.flags = 0xf, .movement = 3};
    const glm::vec3 camera(100.0f, 50.0f, -20.0f);
    swarm.update(0.016f, camera, still);
    REQUIRE(swarm.motes().size() == wm::kCount);
    const auto within = [&] {
        for (const auto& m : swarm.motes())
            for (int i = 0; i < 3; ++i)
                if (std::fabs(m.at[i]) > wm::kSide * 0.5f + 1e-3f) return false;
        return true;
    };
    CHECK(within());
    float least = 1.0f, most = 0.0f;
    for (const auto& m : swarm.motes()) {
        least = std::min(least, m.size);
        most = std::max(most, m.size);
    }
    CHECK(least >= wm::kSize * 0.5f);
    CHECK(most <= wm::kSize * 1.5f);
    CHECK(most > wm::kSize * 1.4f);
    // The camera goes on: they stay where they are in the water, or come in on the other side.
    const std::vector<wm::Mote> before = swarm.motes();
    swarm.update(0.016f, camera + glm::vec3(2.0f, 0.0f, 0.0f), still);
    CHECK(within());
    for (size_t i = 0; i < before.size(); i += 97) {
        const float moved = before[i].at.x - swarm.motes()[i].at.x;
        CHECK((moved == Catch::Approx(2.0f).margin(1e-3) || moved == Catch::Approx(2.0f - wm::kSide).margin(1e-3)));
        CHECK(swarm.motes()[i].at.y == Catch::Approx(before[i].at.y));
    }
    // Far: others.
    swarm.update(0.016f, camera + glm::vec3(0.0f, 500.0f, 0.0f), still);
    CHECK(within());
    CHECK(swarm.motes()[0].size != before[0].size);
}

TEST_CASE("the motes of a liquid sink, rise or sway as its row says", "[water_motes]") {
    const auto moved = [](uint32_t movement, float seconds) {
        wm::Swarm swarm(3);
        const wm::Liquid liquid{.flags = 0xf, .movement = movement};
        swarm.update(0.0f, {}, liquid);
        const glm::vec3 before = swarm.motes()[0].at;
        for (float t = 0.0f; t < seconds; t += 0.02f) swarm.update(0.02f, {}, liquid);
        return swarm.motes()[0].at - before;
    };
    CHECK(moved(1, 1.0f).z == Catch::Approx(-0.02f).margin(1e-3));
    CHECK(moved(2, 1.0f).z == Catch::Approx(0.02f).margin(1e-3));
    CHECK(glm::length(moved(3, 1.0f)) == 0.0f);
    CHECK(glm::length(moved(0, 4.0f)) > 0.01f);
}

TEST_CASE("a mote's picture", "[water_motes]") {
    CHECK(wm::cell(0, 0) == glm::vec2(0.0f));
    CHECK(wm::cell(0, 5) == glm::vec2(1.0f, 1.0f) * wm::kCellSide);
    CHECK(wm::cell(1, 3) == glm::vec2(3.0f, 2.0f) * wm::kCellSide);
    CHECK(wm::cell(2, 6) == glm::vec2(4.0f, 0.0f) * wm::kCellSide);
    CHECK(wm::cell(3, 7) == glm::vec2(4.0f, 4.0f) * wm::kCellSide);
    CHECK(wm::cell(9, 0) == wm::cell(3, 0));
    CHECK(wm::Swarm::seen({0.5f, -0.5f, 1.0f}));
    CHECK_FALSE(wm::Swarm::seen({1.5f, 0.0f, 1.0f}));
    CHECK_FALSE(wm::Swarm::seen({0.0f, 0.0f, -1.0f}));
    CHECK(wm::Liquid{.flags = 0x78}.fogged());
    CHECK_FALSE(wm::Liquid{.flags = 0x142}.motes());
}
