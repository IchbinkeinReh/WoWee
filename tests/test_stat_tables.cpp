// What a unit's stats give it by the combat game tables (game/stat_tables.hpp).
#include <catch_amalgamated.hpp>

#include "game/stat_tables.hpp"

using namespace wowee::game;
namespace st = wowee::game::stat_tables;

namespace {
std::vector<float> column(size_t rows, float value) { return std::vector<float>(rows, value); }
// A mage's pet, as T99's imp: class 8, level 80.
const UnitStats kImp{.classId = 8, .level = 80, .agility = 79, .intellect = 479, .spirit = 367};
constexpr size_t kImpRow = 7 * 100 + 79;
}  // namespace

TEST_CASE("a unit's crit is its class's base and its stat at its level", "[stat_tables]") {
    auto base = column(11, 0.5f);
    base[7] = 0.0345f;
    auto ratio = column(1100, 9.0f);
    ratio[kImpRow] = 0.000196f;
    CHECK(st::critPercent(base, ratio, kImp, kImp.agility) == Catch::Approx((0.000196 * 79 + 0.0345) * 100).margin(1e-4));
    CHECK(st::critPercent(base, ratio, kImp, kImp.intellect) == Catch::Approx((0.000196 * 479 + 0.0345) * 100).margin(1e-4));
    // A stat below nothing is nothing.
    CHECK(st::critPercent(base, ratio, kImp, -5) == Catch::Approx(3.45).margin(1e-4));
    // No ratio, no crit, the base included.
    ratio[kImpRow] = 0.0f;
    CHECK(st::critPercent(base, ratio, kImp, 79) == 0.0f);
    // Nobody.
    ratio[kImpRow] = 1.0f;
    CHECK(st::critPercent(base, ratio, UnitStats{}, 79) == 0.0f);
    UnitStats odd = kImp;
    odd.classId = 12;
    CHECK(st::critPercent(column(12, 1.0f), column(1300, 1.0f), odd, 1) == 0.0f);
    // Past the tables' last level it is that.
    UnitStats above = kImp;
    above.level = 255;
    ratio[7 * 100 + 99] = 0.5f;
    CHECK(st::critPercent(base, ratio, above, 1) == Catch::Approx(53.45).margin(1e-3));
    CHECK(st::critPercent(base, column(10, 1.0f), kImp, 79) == 0.0f);
}

TEST_CASE("the first fifty of spirit regenerate by a table of their own", "[stat_tables]") {
    auto first = column(1100, 0.0f);
    auto rest = column(1100, 0.0f);
    first[kImpRow] = 0.2f;
    rest[kImpRow] = 0.03f;
    CHECK(st::healthRegen(first, rest, kImp) == Catch::Approx(50 * 0.2 + 317 * 0.03).margin(1e-3));
    UnitStats few = kImp;
    few.spirit = 20;
    CHECK(st::healthRegen(first, rest, few) == Catch::Approx(4.0).margin(1e-4));
    few.spirit = -1;
    CHECK(st::healthRegen(first, rest, few) == 0.0f);
    CHECK(st::healthRegen(first, column(5, 1.0f), kImp) == 0.0f);
}

TEST_CASE("the mana of spirit goes by the root of the intellect", "[stat_tables]") {
    auto per = column(1100, 0.0f);
    per[kImpRow] = 0.003345f;
    CHECK(st::manaRegen(per, kImp) == Catch::Approx(std::sqrt(479.0) * 367 * 0.003345).margin(1e-3));
    UnitStats dull = kImp;
    dull.intellect = -3;
    CHECK(st::manaRegen(per, dull) == 0.0f);
    CHECK(st::manaRegen(column(3, 1.0f), kImp) == 0.0f);
}

// 0x0071b8d0.
TEST_CASE("a pet's health is what its creature's template makes of it", "[stat_tables]") {
    CHECK(templateModifier(false, true, 0.85f) == Catch::Approx(0.85));
    CHECK(templateModifier(true, true, 0.85f) == 1.0);
    CHECK(templateModifier(false, false, 0.85f) == 0.0);
}
