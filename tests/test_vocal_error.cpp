// The player's spoken errors (audio/vocal_error.hpp): which game errors
// speak, and the VocalUISounds enum each hands 0x00986d00.
#include <catch_amalgamated.hpp>

#include "audio/vocal_error.hpp"

using wowee::audio::PlayerErrorSpeech;
namespace ve = wowee::audio::vocal_error;

TEST_CASE("a game error speaks the client's line for it", "[audio]") {
    CHECK(ve::forGameError("ERR_OUT_OF_MANA") == PlayerErrorSpeech::NO_MANA);
    CHECK(static_cast<int>(PlayerErrorSpeech::NO_MANA) == 15);
    // Out of range for an item's or object's use and for a spell are two
    // lines, though most races read the same files for both.
    CHECK(ve::forGameError("ERR_OUT_OF_RANGE") == PlayerErrorSpeech::OUT_OF_RANGE);
    CHECK(ve::forGameError("ERR_SPELL_OUT_OF_RANGE") == PlayerErrorSpeech::SPELL_OUT_OF_RANGE);
    CHECK(static_cast<int>(PlayerErrorSpeech::SPELL_OUT_OF_RANGE) == 46);
    // A missing proficiency is its own line, not "I can't use that".
    CHECK(ve::forGameError("ERR_PROFICIENCY_NEEDED") == PlayerErrorSpeech::PROFICIENCY_NEEDED);
    // Several errors share one.
    CHECK(ve::forGameError("ERR_CANT_EQUIP_NEED_TALENT") == PlayerErrorSpeech::CANT_EQUIP_SKILL);
    CHECK(ve::forGameError("ERR_MAIL_BOUND_ITEM") == PlayerErrorSpeech::CANT_TRADE_SOULBOUND);
    CHECK(ve::forGameError("ERR_CURRENCY_FULL") == PlayerErrorSpeech::INVENTORY_FULL);
}

TEST_CASE("most game errors say nothing", "[audio]") {
    CHECK_FALSE(ve::forGameError("ERR_BANK_FULL"));
    CHECK_FALSE(ve::forGameError("ERR_SPELL_FAILED_S"));
    CHECK_FALSE(ve::forGameError("ERR_WRONG_BAG_TYPE"));
    CHECK_FALSE(ve::forGameError(""));
}
