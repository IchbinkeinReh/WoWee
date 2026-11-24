// GetCursorInfo of a spell (0x00515200).
#include <catch_amalgamated.hpp>

#include "game/cursor_info.hpp"

#include <string>

namespace ci = wowee::game::cursor_info;

TEST_CASE("a spell is its place in its book, a companion its place among its kind", "[cursor]") {
    const std::vector<uint32_t> critters = {10, 11}, mounts = {23161, 32243}, book = {6603, 686, 348, 686},
                                pet = {3110, 47241};
    const auto said = [&](uint32_t id) {
        const auto s = ci::spell(id, critters, mounts, book, pet);
        return std::string(s.kind) + " " + std::to_string(s.place) + " " + s.list;
    };
    CHECK(said(6603) == "spell 1 spell");
    CHECK(said(348) == "spell 3 spell");
    // Looked for from the end.
    CHECK(said(686) == "spell 4 spell");
    CHECK(said(47241) == "spell 2 pet");
    CHECK(said(23161) == "companion 1 MOUNT");
    CHECK(said(11) == "companion 2 CRITTER");
    // In no book any more.
    CHECK(said(99) == "spell 0 spell");
    CHECK(ci::placeOf({0, 0}, 0) == 0);
    CHECK(ci::placeOf({}, 5) == 0);
}
