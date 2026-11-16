#include <catch_amalgamated.hpp>

#include "game/combat_text_filter.hpp"
#include "game/world_text.hpp"

// The floating text over units (WorldText.cpp): the table at 0x00af4748 and
// what 0x007e6850, 0x007e6cc0, 0x007e6590 and 0x007e70d0 make of it.

namespace wt = wowee::game::world_text;
using wowee::game::CombatTextEntry;
using wowee::game::floaterKindOf;

TEST_CASE("each kind lives, fades and rises as its table row says", "[world_text]") {
    CHECK(wt::kindOf(0).lifeMs == 1500);
    CHECK(wt::kindOf(5).lifeMs == 4500);
    CHECK(wt::expired(0, 1500));
    CHECK_FALSE(wt::expired(0, 1499));
    CHECK_FALSE(wt::expired(4, 4000));
    // Damage rises two yards over its life; a crit stays put; kind 8 six.
    CHECK(wt::riseAt(0, 750) == Catch::Approx(1.0f));
    CHECK(wt::riseAt(2, 750) == 0.0f);
    CHECK(wt::riseAt(8, 1500) == Catch::Approx(6.0f));
}

TEST_CASE("the alpha fades in, holds and fades out", "[world_text]") {
    CHECK(wt::alphaAt(0, 0).text == 0);
    CHECK(wt::alphaAt(0, 75).text == 128);
    CHECK(wt::alphaAt(0, 75).shadow == 64);
    CHECK(wt::alphaAt(0, 500).text == 255);
    CHECK(wt::alphaAt(0, 500).shadow == 127);
    // From 760 to 1500 ms down to nothing.
    CHECK(wt::alphaAt(0, 1130).text == 128);
    CHECK(wt::alphaAt(0, 1130).shadow == 64);
    CHECK(wt::alphaAt(0, 1499).text <= 1);
    // An absorb starts fading at 90 ms, inside its fade-in: the fade-in wins
    // until it ends.
    CHECK(wt::alphaAt(1, 100).text == 170);
    CHECK(wt::alphaAt(1, 150).text < 255);
}

TEST_CASE("a crit pops from a tenth to twice its size and settles", "[world_text]") {
    CHECK(wt::critPop(0.0f) == Catch::Approx(0.1f));
    CHECK(wt::critPop(0.05f) == Catch::Approx(1.05f));
    CHECK(wt::critPop(0.1f) == Catch::Approx(2.0f));
    CHECK(wt::critPop(0.15f) == Catch::Approx(1.5f));
    CHECK(wt::critPop(0.5f) == Catch::Approx(1.0f));
    CHECK(wt::scaleAt(2, 150) == Catch::Approx(2.0f * wt::kLarge));
    CHECK(wt::scaleAt(7, 1000) == Catch::Approx(wt::kLarge));
    CHECK(wt::scaleAt(0, 700) == Catch::Approx(wt::kSmall));
}

TEST_CASE("sizes are of a screen whose diagonal is one", "[world_text]") {
    // 4:3 at 768 lines: the client's height is 0.6, so 0.0183 is 23.5 px.
    CHECK(wt::pixels(wt::kSmall, 1024.0f, 768.0f) == Catch::Approx(0.018333333f / 0.6f * 768.0f));
}

TEST_CASE("the text stays on the screen", "[world_text]") {
    CHECK(wt::clampCentre(500.0f, 50.0f, 1000.0f) == 500.0f);
    CHECK(wt::clampCentre(10.0f, 50.0f, 1000.0f) == 50.0f);
    CHECK(wt::clampCentre(990.0f, 50.0f, 1000.0f) == 950.0f);
}

TEST_CASE("the rank badge is PvPRank01 for rank 5", "[world_text]") {
    CHECK(wt::rankBadge(4) == 0);
    CHECK(wt::rankBadge(5) == 1);
    CHECK(wt::rankBadge(19) == 15);
    CHECK(wt::rankBadge(20) == 0);
}

TEST_CASE("each floater is drawn as the client's kind for it", "[world_text]") {
    CHECK(floaterKindOf(CombatTextEntry::MELEE_DAMAGE) == 0);
    CHECK(floaterKindOf(CombatTextEntry::CRIT_DAMAGE) == 2);
    CHECK(floaterKindOf(CombatTextEntry::HEAL) == 6);
    CHECK(floaterKindOf(CombatTextEntry::CRIT_HEAL) == 7);
    CHECK(floaterKindOf(CombatTextEntry::ABSORB) == 1);
    CHECK(floaterKindOf(CombatTextEntry::DODGE) == 3);
}

TEST_CASE("Each kind has the client's colour where its maker gives none", "[world_text]") {
    // 0x009d04f0 fills +0x18 of each entry.
    CHECK(wt::colourOf(0) == 0xffffffffu);
    CHECK(wt::colourOf(4) == 0x8094008bu);
    CHECK(wt::colourOf(5) == 0xffe0ca0au);
    CHECK(wt::colourOf(6) == 0xff00ff00u);
    CHECK(wt::colourOf(7) == 0xff00ff00u);
    CHECK(wt::colourOf(10) == 0xffff9900u);
}

TEST_CASE("A swing's colour is its kind's, a pet's swing orange, any spell yellow", "[world_text]") {
    using wowee::game::FloaterSource;
    using wowee::game::floaterColour;
    CHECK(floaterColour(CombatTextEntry::MELEE_DAMAGE, FloaterSource::Player, true) == 0u);
    CHECK(floaterColour(CombatTextEntry::CRIT_DAMAGE, FloaterSource::PlayersUnit, true) == wt::kPetSwingColour);
    CHECK(floaterColour(CombatTextEntry::SPELL_DAMAGE, FloaterSource::Player, false) == wt::kSpellColour);
    CHECK(floaterColour(CombatTextEntry::DODGE, FloaterSource::PlayersUnit, false) == wt::kSpellColour);
    CHECK(floaterColour(CombatTextEntry::HEAL, FloaterSource::Player, false) == 0u);
}

TEST_CASE("A line made above the top of the view is lowered under it by its rise", "[world_text]") {
    // A top plane sloping down away from the viewer: inside below z = 10 - x/10.
    const float plane[4] = {-0.1f, 0.0f, -1.0f, 10.0f};
    CHECK(wt::zUnderTopOfView(plane, 0.0f, 0.0f, 5.0f, 0) == Catch::Approx(5.0f));
    // Above it: to the plane's height less kind 0's two yards.
    CHECK(wt::zUnderTopOfView(plane, 0.0f, 0.0f, 12.0f, 0) == Catch::Approx(8.0f));
    CHECK(wt::zUnderTopOfView(plane, 20.0f, 0.0f, 12.0f, 4) == Catch::Approx(8.0f));  // kind 4 does not rise
    const float upright[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    CHECK(wt::zUnderTopOfView(upright, 0.0f, 0.0f, 50.0f, 0) == Catch::Approx(50.0f));
}
