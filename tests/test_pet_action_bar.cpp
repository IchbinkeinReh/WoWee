// The pet bar's slots as 0x005d6020 moves actions between them.
#include <catch_amalgamated.hpp>

#include "game/pet_action_bar.hpp"

namespace pab = wowee::game::pet_action_bar;

namespace {
constexpr uint32_t spell(uint32_t id) { return (pab::kTypeSpell << 24) | id; }
constexpr uint32_t command(uint32_t v) { return (pab::kTypeCommand << 24) | v; }
}  // namespace

TEST_CASE("a spell put on an empty slot displaces nothing", "[petbar]") {
    pab::Slots s{};
    s[3] = spell(0);
    const auto r = pab::place(s, 3, spell(100));
    CHECK(r.changed);
    CHECK(r.moved == -1);
    CHECK(s[3] == spell(100));
    CHECK(r.displaced == spell(0));
}

TEST_CASE("the same action elsewhere swaps with the slot's old one", "[petbar]") {
    pab::Slots s{};
    s[1] = spell(100);
    s[4] = spell(200);
    const auto r = pab::place(s, 4, spell(100));
    CHECK(r.changed);
    CHECK(r.moved == 1);
    CHECK(s[1] == spell(200));
    CHECK(s[4] == spell(100));
    CHECK(r.displaced == 0);
}

TEST_CASE("a command keeps its place on the bar by moving to an empty slot", "[petbar]") {
    pab::Slots s{};
    for (auto& v : s) v = spell(1);
    s[0] = command(1);  // attack
    s[6] = spell(0);    // the first empty one
    const auto r = pab::place(s, 0, spell(300));
    CHECK(r.changed);
    CHECK(r.moved == 6);
    CHECK(s[6] == command(1));
    CHECK(s[0] == spell(300));
}

TEST_CASE("a command with nowhere to go refuses the placing", "[petbar]") {
    pab::Slots s{};
    for (auto& v : s) v = spell(1);
    s[0] = command(1);
    const auto before = s;
    CHECK_FALSE(pab::place(s, 0, spell(300)).changed);
    CHECK(s == before);
}

TEST_CASE("the action already in the slot changes nothing", "[petbar]") {
    pab::Slots s{};
    s[2] = spell(5);
    CHECK_FALSE(pab::place(s, 2, spell(5)).changed);
}
