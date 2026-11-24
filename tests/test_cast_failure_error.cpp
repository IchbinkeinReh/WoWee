// The game error a player's failed cast goes through (game/cast_failure_error.hpp),
// and the line the character speaks for it (0x00807f10 -> 0x005216f0 -> 0x00986d00).
#include <catch_amalgamated.hpp>

#include <string>
#include <utility>
#include <vector>

#include "audio/vocal_error.hpp"
#include "game/cast_failure_error.hpp"
#include "game/cast_failure_names.hpp"

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

TEST_CASE("a failure repeating the last is held three seconds from each repeat", "[cast_failure]") {
    cf::RepeatHold hold;
    // The first is reported; the same again at once is not.
    CHECK(hold.report(133, 0x43, 1000));
    CHECK_FALSE(hold.report(133, 0x43, 1500));
    // Each repeat takes the time again: 2.9 s after the last one is still held,
    // though 4.4 s after the first.
    CHECK_FALSE(hold.report(133, 0x43, 4400));
    // Three seconds without, and it is said again.
    CHECK(hold.report(133, 0x43, 7400));
}

TEST_CASE("another spell or reason is reported, and a cast going off forgets", "[cast_failure]") {
    cf::RepeatHold hold;
    CHECK(hold.report(133, 0x43, 0));
    CHECK(hold.report(133, 0x55, 100));   // another reason
    CHECK(hold.report(116, 0x55, 200));   // another spell
    CHECK(hold.report(133, 0x55, 300));   // the one before last, again
    CHECK_FALSE(hold.report(133, 0x55, 400));
    hold.forget();
    CHECK(hold.report(133, 0x55, 500));
}

namespace {

// A table as the names are read out of it.
struct Table {
    uint32_t fields;
    struct Row {
        std::vector<uint32_t> numbers;
        std::vector<std::pair<uint32_t, std::string>> words;   // column, word
    };
    std::vector<Row> rows;
    [[nodiscard]] uint32_t getFieldCount() const { return fields; }
    [[nodiscard]] uint32_t getRecordCount() const { return static_cast<uint32_t>(rows.size()); }
    [[nodiscard]] uint32_t getUInt32(uint32_t r, uint32_t f) const { return rows[r].numbers[f]; }
    [[nodiscard]] std::string getString(uint32_t r, uint32_t f) const {
        for (const auto& [column, word] : rows[r].words)
            if (column == f) return word;
        return {};
    }
    [[nodiscard]] int32_t findRecordById(uint32_t id) const {
        for (size_t r = 0; r < rows.size(); ++r)
            if (rows[r].numbers[0] == id) return static_cast<int32_t>(r);
        return -1;
    }
};

}  // namespace

// 0x00808200, case 0x1d to 0x1f.
TEST_CASE("the weapons a spell wants are named as the client names them", "[cast_failure]") {
    const Table masks{3, {{{2, 173555}, {{2, "Nahkampfwaffe"}}}, {{4, 96}, {{2, "Schild"}}}}};
    const Table sub{44, {{{0, 0}, {{10, "Verbrauchbar"}}},
                         {{2, 0}, {{10, "Einhandaxt"}}},
                         {{2, 15}, {{10, "Dolch"}}},
                         {{6, 2}, {{10, "Pfeil"}}},
                         {{6, 3}, {{10, "Kugel"}}}}};
    CHECK(cf::itemClassName(&masks, &sub, 2, 173555) == "Nahkampfwaffe");
    // Not the mask of a row: the first subclass in it.
    CHECK(cf::itemClassName(&masks, &sub, 2, 1u << 15) == "Dolch");
    CHECK(cf::itemClassName(&masks, &sub, 2, (1u << 15) | 1u) == "Einhandaxt");
    CHECK(cf::itemClassName(&masks, &sub, 4, 173555) == "");   // another class's mask
    CHECK(cf::itemClassName<Table>(nullptr, &sub, 6, 1u << 3) == "Kugel");
    CHECK(cf::itemClassName<Table>(nullptr, nullptr, 2, 1) == "");
}

// Case 0x5e, 0x007fe850, 0x0061bd00.
TEST_CASE("the forms a spell allows are joined in their order", "[cast_failure]") {
    const Table forms{35, {{{1}, {{2, "Katzengestalt"}}}, {{2}, {}}, {{3}, {{2, "Reisegestalt"}}}}};
    const uint32_t both[2] = {0b101, 0}, unnamed[2] = {0b010, 0}, none[2] = {0, 0}, high[2] = {0, 1};
    CHECK(cf::formNames(&forms, both) == "Katzengestalt, Reisegestalt");
    CHECK(cf::formNames(&forms, both, 1) == "Katzengestalt");   // a pet's spell names one
    CHECK(cf::formNames(&forms, unnamed) == "");
    CHECK(cf::formNames(&forms, none) == "");
    CHECK(cf::formNames(&forms, high) == "");
}

TEST_CASE("a mechanic, a limit and a pet's text", "[cast_failure]") {
    const Table mechanics{18, {{{2}, {{1, "entwaffnet"}}}}};
    CHECK(cf::rowName(&mechanics, 2) == "entwaffnet");
    CHECK(cf::rowName(&mechanics, 3) == "");
    CHECK(cf::rowName(&mechanics, 0) == "");
    std::vector<uint32_t> row(20, 0);
    row[0] = 2;
    row[18] = 3;
    const Table limits{20, {{row, {{1, "Juwelenschleiferedelsteine"}}}}};
    const auto limit = cf::limitCategory(&limits, 2);
    REQUIRE(limit.has_value());
    CHECK(limit->count == 3);
    CHECK(limit->name == "Juwelenschleiferedelsteine");
    CHECK_FALSE(cf::limitCategory(&limits, 0).has_value());
    CHECK_FALSE(cf::limitCategory(&limits, 9).has_value());
    CHECK(cf::hasPetText(0x39, 0));
    CHECK(cf::hasPetText(0x24, 5));
    CHECK_FALSE(cf::hasPetText(0x24, 1));
    CHECK_FALSE(cf::hasPetText(0x02, 5));
}
