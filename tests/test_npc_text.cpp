// The gossip greeting option shown as 0x0058ac30 picks it: weighted by the
// options' probabilities, the female text for a female unit.
#include <catch_amalgamated.hpp>

#include "game/npc_text.hpp"

namespace nt = wowee::game::npc_text;

namespace {
nt::NpcText twoOptions() {
    nt::NpcText t;
    t.options[0].probability = 0.25f;
    t.options[0].text = {"Hello, sir.", "Hello, ma'am."};
    t.options[1].probability = 0.75f;
    t.options[1].text = {"Greetings.", "Greetings, lady."};
    return t;
}
}  // namespace

TEST_CASE("Options are drawn by their probabilities", "[npc_text]") {
    const auto t = twoOptions();
    // Threshold (1 - roll) * 1.0: under 0.25 is the first, above it the second.
    CHECK(nt::choose(t, 0, 0.9f) == 0);   // threshold 0.1
    CHECK(nt::choose(t, 0, 0.75f) == 0);  // threshold 0.25, reached exactly
    CHECK(nt::choose(t, 0, 0.5f) == 1);   // threshold 0.5
    CHECK(nt::choose(t, 0, 0.0f) == 1);   // threshold 1.0
}

TEST_CASE("Options without text in the column are passed over", "[npc_text]") {
    nt::NpcText t = twoOptions();
    t.options[0].text[1].clear();
    CHECK(nt::choose(t, 1, 0.9f) == 1);
    nt::NpcText none;
    CHECK(nt::choose(none, 0, 0.5f) == -1);
    // Text with no weight at all is still shown.
    nt::NpcText zero;
    zero.options[2].text[0] = "Hm?";
    CHECK(nt::choose(zero, 0, 0.3f) == 2);
}

TEST_CASE("The column follows the unit's gender", "[npc_text]") {
    const auto t = twoOptions();
    CHECK(nt::column(t, true, 1) == 1);
    CHECK(nt::column(t, true, 0) == 0);
    CHECK(nt::column(t, true, 2) == 0);
    CHECK(nt::column(t, false, 1) == 0);
    nt::NpcText femaleOnly;
    femaleOnly.options[0].text[1] = "Only her.";
    CHECK(nt::column(femaleOnly, false, 0) == 1);
}
