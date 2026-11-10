// Waves from a swimmer's limbs at the surface.

#include "rendering/swim_wave.hpp"

#include <catch_amalgamated.hpp>

#include <algorithm>

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
    for (size_t i = 0; i < kMaxWaves + 72; ++i) limbMoved(waves, limb, glm::vec3(x += 0.2f, 0.0f, 10.0f), 10.0f, 0.2f);
    REQUIRE(waves.size() == kMaxWaves);
}

TEST_CASE("the character's ring takes a unit's oldest place once the rings are full", "[swim_wave]") {
    std::vector<Wave> waves(kMaxWaves);
    waves[5].owner = 7;
    Limb limb;
    limbMoved(waves, limb, glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 0.1f);
    REQUIRE(limbMoved(waves, limb, glm::vec3(1.0f, 0.0f, 10.0f), 10.0f, 0.1f) == Made::Ring);
    REQUIRE(waves.size() == kMaxWaves);
    REQUIRE(std::none_of(waves.begin(), waves.end(), [](const Wave& w) { return w.owner == 7; }));
}

TEST_CASE("another unit's limb makes rings within its share", "[swim_wave]") {
    std::vector<Wave> waves;
    Limb limb;
    float x = 0.0f;
    unitLimbMoved(waves, limb, glm::vec3(x, 0.0f, 10.0f), 10.0f, 0.1f, 9, 4, 1);
    size_t made = 0, over = 0;
    // A limb alone with a share of 4 waits kLife / 4 between rings, so it
    // keeps to the share, all but rarely without being turned away.
    for (int i = 0; i < 100; ++i) {
        const Made m = unitLimbMoved(waves, limb, glm::vec3(x += 0.2f, 0.0f, 10.0f), 10.0f, 0.05f, 9, 4, 1);
        made += m == Made::Ring ? 1u : 0u;
        over += m == Made::OverShare ? 1u : 0u;
        age(waves, 0.05f);
    }
    REQUIRE(made >= 10);
    REQUIRE(over <= 2);
    REQUIRE(waves.size() <= 4);
    REQUIRE(waves.back().owner == 9);
}

TEST_CASE("another unit's limb makes no ring once its share or the units' part is spent", "[swim_wave]") {
    std::vector<Wave> waves(kMaxWaves - kOwnRings);
    for (Wave& w : waves) w.owner = 3;
    Limb limb;
    unitLimbMoved(waves, limb, glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 0.1f, 9, 10, 3);
    REQUIRE(unitLimbMoved(waves, limb, glm::vec3(1.0f, 0.0f, 10.0f), 10.0f, 0.1f, 9, 10, 3) == Made::Full);
    REQUIRE(waves.size() == kMaxWaves - kOwnRings);
    // The character's own part is still there for it.
    Limb own;
    limbMoved(waves, own, glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 0.1f);
    REQUIRE(limbMoved(waves, own, glm::vec3(1.0f, 0.0f, 10.0f), 10.0f, 0.1f) == Made::Ring);
    REQUIRE(waves.size() == kMaxWaves - kOwnRings + 1);

    std::vector<Wave> mine(2);
    for (Wave& w : mine) w.owner = 9;
    Limb l2;
    unitLimbMoved(mine, l2, glm::vec3(0.0f, 0.0f, 10.0f), 10.0f, 0.1f, 9, 2, 3);
    REQUIRE(unitLimbMoved(mine, l2, glm::vec3(1.0f, 0.0f, 10.0f), 10.0f, 0.1f, 9, 2, 3) == Made::OverShare);
    REQUIRE(mine.size() == 2);
}

TEST_CASE("a ring ends", "[swim_wave]") {
    std::vector<Wave> waves(1);
    age(waves, kLife * 0.5f);
    REQUIRE(waves.size() == 1);
    age(waves, kLife);
    REQUIRE(waves.empty());
}

// How a crowd in the traced water shares the rings and the trail.
#include "rendering/water_unit_share.hpp"

namespace share = wowee::rendering::water_share;

TEST_CASE("a crowd out to 150 yards all get a share, nearer ones more", "[swim_wave]") {
    std::vector<float> yards;
    for (int i = 0; i < 12; ++i) yards.push_back(5.0f + 12.0f * static_cast<float>(i));  // 5 .. 137
    const auto s = share::shareOut(yards, kMaxWaves - kOwnRings, 192);
    size_t rings = 0, points = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        REQUIRE(s[i].rings >= share::kMinRings);
        REQUIRE(s[i].wakePoints >= share::kMinWakePoints);
        if (i > 0) REQUIRE(s[i].rings <= s[i - 1].rings + 1);
        rings += s[i].rings;
        points += s[i].wakePoints;
    }
    REQUIRE(rings <= kMaxWaves - kOwnRings);
    REQUIRE(points <= 192);
    REQUIRE(s.front().rings > s.back().rings);
}

TEST_CASE("one unit alone has its natural rings and trail", "[swim_wave]") {
    const auto s = share::shareOut({40.0f}, kMaxWaves - kOwnRings, 192);
    REQUIRE(s[0].rings == share::kMostRings);
    REQUIRE(s[0].wakePoints == share::kMostWakePoints);
}

TEST_CASE("a crowd too big for a few each still gives every unit a ring and a trail", "[swim_wave]") {
    // Forty, then a hundred units out to 150 yards: more than the pool holds
    // at three rings each, and every one of them still has some.
    for (int count : {40, 100}) {
        std::vector<float> yards;
        for (int i = 0; i < count; ++i) yards.push_back(150.0f * static_cast<float>(i + 1) / static_cast<float>(count));
        const auto s = share::shareOut(yards, kMaxWaves - kOwnRings, 448);
        size_t rings = 0, points = 0;
        for (const auto& u : s) {
            REQUIRE(u.rings >= share::kFewestRings);
            REQUIRE(u.wakePoints >= share::kFewestWakePoints);
            rings += u.rings;
            points += u.wakePoints;
        }
        REQUIRE(rings <= kMaxWaves - kOwnRings);
        REQUIRE(points <= 448);
        // Nearer ones still more.
        REQUIRE(s.front().rings >= s.back().rings);
    }
}

TEST_CASE("past one ring each the farthest go without", "[swim_wave]") {
    std::vector<float> yards(120, 100.0f);
    const auto s = share::shareOut(yards, 96, 192);
    size_t rings = 0;
    for (const auto& u : s) rings += u.rings;
    REQUIRE(rings <= 96);
    REQUIRE(s.front().rings == share::kFewestRings);
    REQUIRE(s[95].rings == share::kFewestRings);
    REQUIRE(s.back().rings == 0);
}

TEST_CASE("points are grouped by owner with the circle they reach", "[swim_wave]") {
    struct P { float x, y; uint64_t o; };
    std::vector<P> pts{{0, 0, 0}, {2, 0, 0}, {100, 0, 5}, {104, 0, 5}, {50, 50, 9}};
    const auto g = share::groupByOwner(pts.size(), 32, 1.0f, [&](size_t i) { return pts[i].o; },
                                       [&](size_t i) { return glm::vec2(pts[i].x, pts[i].y); });
    REQUIRE(g.size() == 3);
    REQUIRE(g[0].first == 0);
    REQUIRE(g[0].x == Catch::Approx(1.0f));
    REQUIRE(g[0].radius == Catch::Approx(2.0f));
    REQUIRE(g[1].first == 2);
    REQUIRE(g[1].radius == Catch::Approx(3.0f));
    REQUIRE(g[2].first == 4);
    const auto two = share::groupByOwner(pts.size(), 2, 1.0f, [&](size_t i) { return pts[i].o; },
                                         [&](size_t i) { return glm::vec2(pts[i].x, pts[i].y); });
    REQUIRE(two.size() == 2);
    REQUIRE(two[1].first == 2);  // the owners past the last group join it
    REQUIRE(two[1].radius > 30.0f);
}
