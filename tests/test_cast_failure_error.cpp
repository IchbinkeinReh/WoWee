// The game error a player's failed cast goes through (game/cast_failure_error.hpp),
// and the line the character speaks for it (0x00807f10 -> 0x005216f0 -> 0x00986d00).
#include <catch_amalgamated.hpp>

#include "audio/vocal_error.hpp"
#include "game/cast_failure_error.hpp"

namespace cf = wowee::game::cast_failure;
using wowee::audio::PlayerErrorSpeech;
using wowee::audio::vocal_error::forGameError;

namespace {
std::optional<PlayerErrorSpeech> spoken(uint8_t result, const cf::SpellRow& spell) {
    return forGameError(cf::gameError(result, spell));
}
}  // namespace

TEST_CASE("a refused cast speaks the client's line for its game error", "[cast_failure]") {
    const cf::SpellRow fireball{};
    CHECK(spoken(0x0b, fireball) == PlayerErrorSpeech::GENERIC_NO_TARGET);
    CHECK(spoken(0xb2, fireball) == PlayerErrorSpeech::GENERIC_NO_TARGET);
    CHECK(spoken(0x34, fireball) == PlayerErrorSpeech::NO_AMMO);
    // Out of a spell's range is the spell's line, not an item's or object's.
    CHECK(spoken(0x61, fireball) == PlayerErrorSpeech::SPELL_OUT_OF_RANGE);
    CHECK(spoken(0x43, fireball) == PlayerErrorSpeech::SPELL_COOLDOWN);
    // An ability (Attributes 0x10) is not ready, a spell is.
    cf::SpellRow mortalStrike{};
    mortalStrike.attributes = cf::kAttrAbility;
    mortalStrike.powerType = 1;
    CHECK(spoken(0x43, mortalStrike) == PlayerErrorSpeech::ABILITY_COOLDOWN);
    CHECK(spoken(0x55, mortalStrike) == PlayerErrorSpeech::NO_RAGE);
}

TEST_CASE("NO_POWER speaks by the spell's power", "[cast_failure]") {
    cf::SpellRow spell{};
    spell.powerType = 0;
    CHECK(spoken(0x55, spell) == PlayerErrorSpeech::NO_MANA);
    spell.powerType = 3;
    CHECK(spoken(0x55, spell) == PlayerErrorSpeech::NO_ENERGY);
    // Focus, happiness, runes and runic power have no line.
    for (int32_t p : {2, 4, 5, 6, -2}) {
        spell.powerType = p;
        CHECK_FALSE(spoken(0x55, spell));
    }
    // A vehicle's displayed power says nothing, energy underneath or not.
    spell.powerType = 3;
    spell.powerDisplay = true;
    CHECK(cf::gameError(0x55, spell) == "ERR_OUT_OF_POWER_DISPLAY");
    CHECK_FALSE(spoken(0x55, spell));
}

TEST_CASE("a potion or food not ready, by the spell's category", "[cast_failure]") {
    cf::SpellRow potion{};
    potion.category = 4;
    CHECK(spoken(0x2d, potion) == PlayerErrorSpeech::POTION_COOLDOWN);
    CHECK(spoken(0x43, potion) == PlayerErrorSpeech::POTION_COOLDOWN);
    cf::SpellRow food{};
    food.category = 11;
    CHECK(cf::gameError(0x2d, food) == "ERR_FOOD_COOLDOWN");
    cf::SpellRow other{};
    other.category = 1;
    CHECK(spoken(0x2d, other) == PlayerErrorSpeech::ITEM_COOLDOWN);
}

TEST_CASE("bad targets speak only for an item target", "[cast_failure]") {
    cf::SpellRow enchant{};
    enchant.targets = cf::kTargetItem;
    CHECK(spoken(0x0c, enchant) == PlayerErrorSpeech::CANT_USE_ITEM);
    cf::SpellRow glyph{};
    glyph.targets = cf::kTargetGlyphSlot;
    CHECK(cf::gameError(0x0c, glyph) == "ERR_INVALID_GLYPH_SLOT");
    CHECK_FALSE(spoken(0x0c, glyph));
    // A unit target: ERR_SPELL_FAILED_S, and nothing said.
    CHECK(cf::gameError(0x0c, cf::SpellRow{}) == "ERR_SPELL_FAILED_S");
    CHECK_FALSE(spoken(0x0c, cf::SpellRow{}));
}

TEST_CASE("most refusals say nothing", "[cast_failure]") {
    const cf::SpellRow spell{};
    // CHEST_IN_USE, NEED_AMMO_POUCH, NEED_EXOTIC_AMMO, NO_AMMO and moving.
    for (uint8_t r : {0x19, 0x35, 0x36, 0x4b, 0x33}) {
        CHECK(cf::gameError(r, spell) == "ERR_SPELL_FAILED_S");
        CHECK_FALSE(spoken(r, spell));
    }
}

TEST_CASE("a pet's refused cast speaks only its cooldown and power lines", "[cast_failure]") {
    cf::SpellRow claw{};
    claw.attributes = cf::kAttrAbility;
    claw.powerType = 2;  // focus
    CHECK(forGameError(cf::petGameError(0x43, claw)) == PlayerErrorSpeech::ABILITY_COOLDOWN);
    CHECK_FALSE(forGameError(cf::petGameError(0x55, claw)));
    cf::SpellRow firebolt{};
    firebolt.powerType = 0;
    CHECK(forGameError(cf::petGameError(0x43, firebolt)) == PlayerErrorSpeech::SPELL_COOLDOWN);
    CHECK(forGameError(cf::petGameError(0x55, firebolt)) == PlayerErrorSpeech::NO_MANA);
    // Its own lines are silent: out of range is ERR_PET_SPELL_OUT_OF_RANGE.
    CHECK(cf::petGameError(0x61, firebolt) == "ERR_PET_SPELL_OUT_OF_RANGE");
    CHECK_FALSE(forGameError(cf::petGameError(0x61, firebolt)));
    // No potion or food variant for a pet.
    cf::SpellRow potion{};
    potion.category = 4;
    CHECK(cf::petGameError(0x43, potion) == "ERR_SPELL_COOLDOWN");
}
