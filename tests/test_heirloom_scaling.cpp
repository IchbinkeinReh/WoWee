// An heirloom's numbers at a level out of one ScalingStatValues row
// (0x0061a650, 0x0061a6b0, 0x0061a830; heirloom_scaling.hpp).
#include <catch_amalgamated.hpp>
#include "game/heirloom_scaling.hpp"

#include <array>

using namespace wowee::game::heirloom;

namespace {
// A row whose every column is its own index times ten, so the column picked
// can be read off the answer.
std::array<int32_t, kScalingStatValuesColumns> indexRow() {
    std::array<int32_t, kScalingStatValuesColumns> r{};
    for (size_t i = 0; i < r.size(); ++i) r[i] = static_cast<int32_t>(i) * 10;
    return r;
}
}  // namespace

TEST_CASE("The stat budget comes from the column the mask's low bits pick", "[heirloom]") {
    const auto r = indexRow();
    CHECK(statBudget(r.data(), r.size(), 0x1) == 20);
    CHECK(statBudget(r.data(), r.size(), 0x2) == 30);
    CHECK(statBudget(r.data(), r.size(), 0x4) == 40);
    CHECK(statBudget(r.data(), r.size(), 0x8) == 170);
    CHECK(statBudget(r.data(), r.size(), 0x10) == 50);
    CHECK(statBudget(r.data(), r.size(), 0x40000) == 180);
    // Two bits at once pick nothing, as the client's switch does.
    CHECK(statBudget(r.data(), r.size(), 0x3) == 0);
    CHECK(scaledStat(5000, 77) == 38);
}

TEST_CASE("Armour by the mask's armour bits, none without them", "[heirloom]") {
    const auto r = indexRow();
    CHECK(scalesArmour(0x20));
    CHECK(armour(r.data(), r.size(), 0x20 | 0x1) == 60);
    CHECK(armour(r.data(), r.size(), 0x100) == 90);
    CHECK(armour(r.data(), r.size(), 0x800000) == 230);
    CHECK_FALSE(scalesArmour(0x1 | 0x200));
}

TEST_CASE("A hit's range is the damage per second's spread times the swing", "[heirloom]") {
    auto r = indexRow();
    r[10] = 100;  // one-hand melee DPS
    r[11] = 100;  // two-hand melee DPS
    float lo = 0, hi = 0;
    damage(r.data(), r.size(), 0x200, 2000, lo, hi);
    CHECK(lo == Catch::Approx(140.0f));  // (100 - 30) x 2.0
    CHECK(hi == Catch::Approx(260.0f));  // (100 + 30) x 2.0
    damage(r.data(), r.size(), 0x400, 3000, lo, hi);
    CHECK(lo == Catch::Approx(240.0f));  // two-hand: 20% either way
    CHECK(hi == Catch::Approx(360.0f));
    CHECK(scalesDamage(0x400));
    CHECK_FALSE(scalesDamage(0x8000));
}

TEST_CASE("Spell power only with its bit", "[heirloom]") {
    const auto r = indexRow();
    CHECK(spellPower(r.data(), r.size(), 0x8000) == 160);
    CHECK(spellPower(r.data(), r.size(), 0x1) == 0);
}
