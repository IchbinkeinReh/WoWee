#include <catch_amalgamated.hpp>

#include "game/gem_conditions.hpp"

#include <cstring>

using namespace wowee::game::gem_conditions;

// SpellItemEnchantmentCondition rows as 3.3.5a ships them; 0x0061c0b0 words
// and tests them.

namespace {

Row single(uint8_t color, uint8_t op, uint8_t compare, int32_t value) {
    Row r;
    r.color[0] = color;
    r.comparator[0] = op;
    r.compareColor[0] = compare;
    r.value[0] = value;
    return r;
}

}  // namespace

TEST_CASE("Row 36: at least two of each colour, chained by Logic", "[gem_conditions]") {
    Row r;
    r.color = {2, 3, 4, 0, 0};
    r.comparator = {5, 5, 5, 0, 0};
    r.value = {2, 2, 2, 0, 0};
    r.logic = {1, 1, 0, 0, 0};

    auto res = evaluate(r, Counts{1, 2, 2, 1});
    REQUIRE(res.lines.size() == 3);
    CHECK(std::strcmp(res.lines[0].key, "ENCHANT_CONDITION_MORE_VALUE") == 0);
    CHECK(res.lines[0].valueForm);
    CHECK(res.lines[0].number == 2);
    CHECK(res.lines[0].met);
    CHECK(res.lines[1].met);
    CHECK_FALSE(res.lines[2].met);   // one blue gem
    CHECK_FALSE(res.allMet);

    CHECK(evaluate(r, Counts{0, 2, 3, 2}).allMet);
}

TEST_CASE("Strictly-more comparators word the bound one higher", "[gem_conditions]") {
    // Comparator 3 is "more than Value": worded MORE_VALUE with Value+1.
    auto res = evaluate(single(4, 3, 0, 2), Counts{0, 0, 0, 3});
    CHECK(std::strcmp(res.lines[0].key, "ENCHANT_CONDITION_MORE_VALUE") == 0);
    CHECK(res.lines[0].number == 3);
    CHECK(res.lines[0].met);
    CHECK_FALSE(evaluate(single(4, 3, 0, 2), Counts{0, 0, 0, 2}).allMet);

    // Comparator 4 is "at most Value": LESS_VALUE with Value+1.
    res = evaluate(single(2, 4, 0, 1), Counts{0, 1, 0, 0});
    CHECK(std::strcmp(res.lines[0].key, "ENCHANT_CONDITION_LESS_VALUE") == 0);
    CHECK(res.lines[0].number == 2);
    CHECK(res.lines[0].met);
    // Comparator 2 is "less than Value" as it stands.
    CHECK_FALSE(evaluate(single(2, 2, 0, 1), Counts{0, 1, 0, 0}).allMet);
}

TEST_CASE("Colour comparisons swap the names for comparators 2 and 4", "[gem_conditions]") {
    // Row 30: blue (4) comparator 3 against yellow (3) - more blue than yellow.
    auto res = evaluate(single(4, 3, 3, 0), Counts{0, 0, 1, 2});
    CHECK(std::strcmp(res.lines[0].key, "ENCHANT_CONDITION_MORE_COMPARE") == 0);
    CHECK(res.lines[0].colorA == 4);
    CHECK(res.lines[0].colorB == 3);
    CHECK(res.lines[0].met);
    CHECK_FALSE(evaluate(single(4, 3, 3, 0), Counts{0, 0, 2, 2}).allMet);

    // Comparator 2: more of CompareColor than of Color, named that way round.
    res = evaluate(single(2, 2, 3, 0), Counts{0, 1, 2, 0});
    CHECK(res.lines[0].colorA == 3);
    CHECK(res.lines[0].colorB == 2);
    CHECK(res.lines[0].met);

    // Comparator 4: at least as many of CompareColor, also swapped.
    res = evaluate(single(2, 4, 3, 0), Counts{0, 2, 2, 0});
    CHECK(std::strcmp(res.lines[0].key, "ENCHANT_CONDITION_MORE_EQUAL_COMPARE") == 0);
    CHECK(res.lines[0].colorA == 3);
    CHECK(res.lines[0].met);
}

TEST_CASE("An unworded comparator passes", "[gem_conditions]") {
    auto res = evaluate(single(2, 9, 0, 1), Counts{});
    CHECK(res.lines[0].key == nullptr);
    CHECK(res.allMet);
}

TEST_CASE("Gem colours count once per bit", "[gem_conditions]") {
    Counts c{};
    addGem(c, 6);   // orange: red and yellow
    addGem(c, 1);   // meta
    CHECK(c == Counts{1, 1, 1, 0});
    CHECK(countOf(c, 0) == 0);
    CHECK(countOf(c, 5) == 0);
    CHECK(colorKey(5) == nullptr);
}
