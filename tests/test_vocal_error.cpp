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

TEST_CASE("a game error with no line plays the sound its row names", "[audio]") {
    CHECK(ve::namedSound("ERR_QUEST_FAILED_S") == std::optional<std::string_view>("igQuestFailed"));
    CHECK(ve::namedSound("ERR_QUEST_LOG_FULL") == std::optional<std::string_view>("igQuestFailed"));
    CHECK(ve::namedSound("ERR_NEWTAXIPATH") == std::optional<std::string_view>("TaxiNodeDiscovered"));
    CHECK(ve::namedSound("ERR_FRIEND_ONLINE_SS") == std::optional<std::string_view>("FRIENDJOINGAME"));
    CHECK(ve::namedSound("ERR_DUEL_REQUESTED") == std::optional<std::string_view>("LEVELUP"));
    // One that speaks has no named sound, and most have neither.
    CHECK_FALSE(ve::namedSound("ERR_OUT_OF_MANA"));
    CHECK_FALSE(ve::namedSound("ERR_FRIEND_OFFLINE_S"));
}
