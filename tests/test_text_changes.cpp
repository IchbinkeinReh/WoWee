// What an edit box owes of OnTextChanged (0x00967020).
#include <catch_amalgamated.hpp>

#include "ui/text_changes.hpp"

#include <map>

using wowee::ui::TextChanges;
using Box = TextChanges::Box;

TEST_CASE("a box is told once, as the last change had it", "[editbox]") {
    TextChanges changes;
    changes.note(7, true);
    changes.note(9, false);
    changes.note(7, false);
    const auto all = [](uint32_t) { return Box::Shown; };
    const auto told = changes.take(all);
    REQUIRE(told.size() == 2);
    CHECK(told[0].box == 7);
    CHECK_FALSE(told[0].userInput);
    CHECK(told[1].box == 9);
    CHECK(changes.take(all).empty());
}

// MoneyInputFrame counts the events it is to ignore: one a box, not one a
// SetNumber.
TEST_CASE("a box hidden is told when it is shown", "[editbox]") {
    TextChanges changes;
    std::map<uint32_t, Box> boxes = {{1, Box::Hidden}, {2, Box::Shown}, {3, Box::Gone}};
    const auto state = [&](uint32_t id) { return boxes[id]; };
    for (uint32_t id : {1u, 2u, 3u}) changes.note(id, false);
    auto told = changes.take(state);
    REQUIRE(told.size() == 1);
    CHECK(told[0].box == 2);
    CHECK(changes.size() == 1);
    changes.note(1, true);
    CHECK(changes.take(state).empty());
    boxes[1] = Box::Shown;
    told = changes.take(state);
    REQUIRE(told.size() == 1);
    CHECK(told[0].userInput);
    CHECK(changes.size() == 0);
}

TEST_CASE("a key finds nothing left to tell", "[editbox]") {
    TextChanges changes;
    changes.note(4, true);
    CHECK(changes.takeOf(4) == true);
    CHECK_FALSE(changes.takeOf(4).has_value());
    changes.note(4, false);
    changes.note(5, true);
    CHECK(changes.takeOf(4) == false);
    CHECK(changes.size() == 1);
    changes.clear();
    CHECK_FALSE(changes.takeOf(5).has_value());
}
