// The $-tokens WoW's server text is written with, and what fills them in
// (0x00579590 -> 0x00578f20).
//
// The server sends a quest, a gossip line or a monster's say with the player
// left as a blank - "$N, you have done well" - and the client writes the name
// in. The class and the race went in in English in every client, and the text
// was read by rules of our own.
#include <catch_amalgamated.hpp>

#include "game/chr_names.hpp"
#include "game/text_tokens.hpp"

using namespace wowee;

namespace {
game::TextSubject nightElfHuntress() {
    game::TextSubject s;
    s.name = "Shandris";
    s.className = "J\xc3\xa4gerin";
    s.raceName = "Nachtelfe";
    s.gender = game::Gender::FEMALE;
    return s;
}
}  // namespace

TEST_CASE("the player's own details", "[text][tokens]") {
    const auto s = nightElfHuntress();
    CHECK(game::resolveTextTokens("$N, you have done well.", s) == "Shandris, you have done well.");
    CHECK(game::resolveTextTokens("Well met, $n.", s) == "Well met, Shandris.");
    CHECK(game::resolveTextTokens("Eine $R und $C.", s) == "Eine Nachtelfe und J\xc3\xa4gerin.");

    SECTION("the small letters' tokens are in small letters (0x0076f6e0)") {
        CHECK(game::resolveTextTokens("A $r $c stands before me.", s) ==
              "A nachtelfe j\xc3\xa4gerin stands before me.");
        CHECK(game::resolveTextTokens("$n", s) == "Shandris");
    }
    SECTION("a line with no token comes back untouched") {
        CHECK(game::resolveTextTokens("Hail, traveller.", s) == "Hail, traveller.");
    }
    SECTION("$b is a line break") {
        CHECK(game::resolveTextTokens("One.$bTwo.$B$BThree.", s) == "One.\nTwo.\n\nThree.");
    }
}

TEST_CASE("$g chooses by gender (0x00576cd0)", "[text][tokens]") {
    const auto her = nightElfHuntress();
    game::TextSubject him = her;
    him.name = "Jarod";
    him.gender = game::Gender::MALE;

    CHECK(game::resolveTextTokens("Well met, $gsir:madam;.", him) == "Well met, sir.");
    CHECK(game::resolveTextTokens("Well met, $gsir:madam;.", her) == "Well met, madam.");
    CHECK(game::resolveTextTokens("$G sir : madam ;!", him) == "sir!");
    CHECK(game::resolveTextTokens("$G sir : madam ;!", her) == "madam!");

    SECTION("by the gender of the class's word, or the race's, where the text says so") {
        // Todesritter is one word for both: a woman's is in the man's form.
        game::TextSubject knight = her;
        knight.className = "Todesritter";
        knight.classGender = game::Gender::MALE;
        knight.raceGender = game::Gender::FEMALE;
        CHECK(game::resolveTextTokens("$gein:eine:c; $C", knight) == "ein Todesritter");
        CHECK(game::resolveTextTokens("$gein:eine:r; $R", knight) == "eine Nachtelfe");
        CHECK(game::resolveTextTokens("$gein:eine;", knight) == "eine");
    }
    SECTION("any third part else leaves the choice to the reader") {
        CHECK(game::resolveTextTokens("$gsir:madam:friend;", her) == "madam");
        CHECK(game::resolveTextTokens("$gsir:madam:friend;", him) == "sir");
    }
    SECTION("a rank of honour nobody has is the same choice") {
        CHECK(game::resolveTextTokens("$tsoldier:lady;", her) == "lady");
    }
}

TEST_CASE("world states, durations and times left", "[text][tokens]") {
    game::TextSubject s = nightElfHuntress();
    s.worldState = [](int32_t id) -> uint32_t {
        switch (id) {
            case 3233: return 47;
            case -12: return 90;
            case -13: return 3000;
            case 77: return 1000u + 3725u;
            default: return 0;
        }
    };
    s.duration = [](char unit, int n) { return std::to_string(n) + unit; };
    s.serverTime = 1000;
    CHECK(game::resolveTextTokens("$3233w%", s) == "47%");
    CHECK(game::resolveTextTokens("$12e", s) == "90");
    CHECK(game::resolveTextTokens("$12d", s) == "3H");  // one and a half and, half an hour past, one more
    CHECK(game::resolveTextTokens("$13d", s) == "3D");
    CHECK(game::resolveTextTokens("$9d", s) == "0M");
    CHECK(game::resolveTextTokens("$77k", s) == "1:02:05");
    CHECK(game::resolveTextTokens("$78k", s) == "00:00");
}

TEST_CASE("a character we know nothing about still reads as a sentence", "[text][tokens]") {
    // Before the character list arrives there is no name to write in, and the
    // line still has to be printable.
    const game::TextSubject unknown;
    CHECK(game::resolveTextTokens("Greetings, $N.", unknown) == "Greetings, Adventurer.");
}

TEST_CASE("a dollar that is no token stays", "[text][tokens]") {
    const auto s = nightElfHuntress();
    // Pronouns are none of this client's.
    CHECK(game::resolveTextTokens("$p $o $s", s) == "$p $o $s");
    CHECK(game::resolveTextTokens("Ends with a $", s) == "Ends with a $");
    CHECK(game::resolveTextTokens("$$N", s) == "$Shandris");
}

TEST_CASE("a race's or a class's word is picked by gender (0x007159e0)", "[text][tokens]") {
    using game::pickGenderedName;
    CHECK(pickGenderedName("Hexenmeister", "Hexenmeisterin", "Hexenmeister", 1).name == "Hexenmeisterin");
    CHECK(pickGenderedName("Hexenmeister", "Hexenmeisterin", "Hexenmeister", 1).gender == 1);
    CHECK(pickGenderedName("Hexenmeister", "Hexenmeisterin", "Hexenmeister", 0).name == "Hexenmeister");
    // One word for both is the man's, and its gender with it.
    CHECK(pickGenderedName("Todesritter", "", "Todesritter", 1).name == "Todesritter");
    CHECK(pickGenderedName("Todesritter", "", "Todesritter", 1).gender == 0);
    CHECK(pickGenderedName("Orc", "", "", 1).name == "Orc");
    CHECK(pickGenderedName("Orc", "", "", 1).gender == 1);
    CHECK(pickGenderedName("Krieger", "Kriegerin", "Krieger", 2).name == "Krieger");
}
