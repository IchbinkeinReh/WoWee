// A refused loot (game/loot_error.hpp): the game error 0x006d53b0 shows for
// the server's reason, and the line the character says for it.
#include <catch_amalgamated.hpp>

#include <string_view>

#include "audio/vocal_error.hpp"
#include "game/loot_error.hpp"

namespace le = wowee::game::loot_error;
using wowee::audio::PlayerErrorSpeech;
using wowee::audio::vocal_error::forGameError;

TEST_CASE("a refused loot is shown by its reason", "[loot_error]") {
    CHECK(std::string_view(le::forReason(4).key) == "ERR_LOOT_TOO_FAR");
    CHECK(std::string_view(le::forReason(5).key) == "ERR_LOOT_BAD_FACING");
    CHECK(std::string_view(le::forReason(6).key) == "ERR_LOOT_LOCKED");
    CHECK(std::string_view(le::forReason(0).key) == "ERR_LOOT_DIDNT_KILL");
    // An unlisted reason, the pickpocket one included, is DIDNT_KILL.
    CHECK(std::string_view(le::forReason(7).key) == "ERR_LOOT_DIDNT_KILL");
    CHECK(std::string_view(le::forReason(le::kAlreadyPickpocketed).key) == "ERR_LOOT_DIDNT_KILL");
    CHECK(std::string_view(le::forReason(16).key) == "ERR_NOT_WHILE_SHAPESHIFTED");
}

TEST_CASE("the loot refusals the character speaks", "[loot_error]") {
    CHECK(forGameError(le::forReason(4).key) == PlayerErrorSpeech::LOOT_TOO_FAR);
    CHECK(forGameError(le::forReason(5).key) == PlayerErrorSpeech::LOOT_BAD_FACING);
    CHECK(forGameError(le::forReason(6).key) == PlayerErrorSpeech::LOOT_LOCKED);
    CHECK(forGameError(le::forReason(0).key) == PlayerErrorSpeech::LOOT_DIDNT_KILL);
    CHECK_FALSE(forGameError(le::forReason(9).key));
    CHECK_FALSE(forGameError(le::alreadyPickpocketed().key));
}
