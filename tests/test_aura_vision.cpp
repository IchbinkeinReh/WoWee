// game::aura_vision: which auras on a unit a player is shown, by the aura
// vision they ask for and the player's (0x00724820, 0x007fe3e0).

#include <catch_amalgamated.hpp>

#include "game/aura_vision.hpp"

using namespace wowee::game::aura_vision;

TEST_CASE("an aura asks for the vision its detection or its spell names", "[aura_vision]") {
    const uint32_t none[3] = {0, 0, 0};
    CHECK(requiredVision(0, none, none) == 0u);
    // RequiredAuraVision n as bit n - 1.
    CHECK(requiredVision(1, none, none) == 0x1u);
    CHECK(requiredVision(7, none, none) == 0x40u);
    // Stealth detection, in any effect.
    const uint32_t stealth[3] = {0, 0, kAuraStealthDetect};
    CHECK(requiredVision(0, stealth, none) == kVisionStealth);
    // Invisibility detection: the ordinary kind (misc 0) and type 10 only.
    const uint32_t invis[3] = {kAuraInvisibilityDetect, 0, 0};
    const uint32_t misc0[3] = {0, 0, 0}, misc10[3] = {10, 0, 0}, misc3[3] = {3, 0, 0};
    CHECK(requiredVision(0, invis, misc0) == kVisionInvisibility);
    CHECK(requiredVision(0, invis, misc10) == kVisionInvisibility);
    CHECK(requiredVision(0, invis, misc3) == 0u);
    // Both together.
    const uint32_t both[3] = {kAuraInvisibilityDetect, kAuraStealthDetect, 0};
    CHECK(requiredVision(0, both, misc0) == (kVisionStealth | kVisionInvisibility));
}

TEST_CASE("a player is shown an aura only with the vision it asks for", "[aura_vision]") {
    CHECK(visibleTo(0, 0));                                   // asks for none
    CHECK_FALSE(visibleTo(kVisionInvisibility, 0));           // the Arcane Guardian's eye
    CHECK(visibleTo(kVisionInvisibility, kVisionInvisibility));
    CHECK_FALSE(visibleTo(kVisionInvisibility, kVisionStealth));
    CHECK(visibleTo(kVisionStealth | kVisionInvisibility, kVisionStealth));
}
