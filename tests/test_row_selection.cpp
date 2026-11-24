#include <catch_amalgamated.hpp>

#include "addons/header_fold.hpp"
#include "addons/row_selection.hpp"

#include <cstdint>
#include <vector>

using wowee::addons::RowSelection;

// The guild roster, the skills, the factions and the recipes keep what is
// selected, not where it was (0x005caf40, 0x005cd860, 0x005d07b0, 0x005da900).
TEST_CASE("A list's selection goes with its row", "[addons][selection]") {
    const auto id = [](uint32_t v) { return v; };
    RowSelection<uint32_t> selected;
    std::vector<uint32_t> rows = {10, 0, 20, 30};
    CHECK(selected.index(rows, id) == 0);
    selected.select(rows, 3, id);
    CHECK(selected.index(rows, id) == 3);
    rows.erase(rows.begin());
    CHECK(selected.index(rows, id) == 2);
    rows.insert(rows.begin(), {1, 2, 3});
    CHECK(selected.index(rows, id) == 5);
    // Filtered out, it has no place, and has it again when it comes back.
    CHECK(selected.index(std::vector<uint32_t>{10, 30}, id) == 0);
    CHECK(selected.index(rows, id) == 5);
    // A row that is nothing to select - a heading - and one off the end.
    selected.select(rows, 6, id);
    CHECK(selected.index(rows, id) == 6);
    selected.select(rows, 2 + 99, id);
    CHECK(selected.index(rows, id) == 0);
    std::vector<uint32_t> heading = {0, 5};
    selected.select(heading, 1, id);
    CHECK(selected.index(heading, id) == 0);
    selected.select(heading, 0, id);
    CHECK(selected.index(heading, id) == 0);
}

TEST_CASE("folding a heading reaches what the client's does", "[fold]") {
    using wowee::addons::FoldReach;
    const std::vector<bool> rows = {true, false, true};
    // 0x005d1d20
    CHECK(wowee::addons::factionFold(rows, 1) == FoldReach::One);
    CHECK(wowee::addons::factionFold(rows, 2) == FoldReach::All);
    CHECK(wowee::addons::factionFold(rows, 0) == FoldReach::All);
    CHECK(wowee::addons::factionFold(rows, 4) == FoldReach::All);
    // 0x005ce3a0: CollapseSkillHeader(0) is the "All" button.
    CHECK(wowee::addons::skillFold(3, 0) == FoldReach::All);
    CHECK(wowee::addons::skillFold(3, 2) == FoldReach::One);
    CHECK(wowee::addons::skillFold(3, 4) == FoldReach::None);
}
