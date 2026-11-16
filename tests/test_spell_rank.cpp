#include <catch_amalgamated.hpp>

#include "game/spell_rank.hpp"

#include <vector>

using namespace wowee::game::spell_rank;

// The rank's number is read out of the text wherever the client's language
// puts it (0x0053b540): a German client's "Rang 13" is 13, not 0.
TEST_CASE("rank number is the first run of digits in any language", "[spell_rank]") {
    REQUIRE(rankNumber("Rank 5") == 5);
    REQUIRE(rankNumber("Rang 13") == 13);
    REQUIRE(rankNumber("Rango 2") == 2);
    REQUIRE(rankNumber("Уровень 7") == 7);
    REQUIRE(rankNumber("") == 0);
    REQUIRE(rankNumber("Passiv") == 0);
    REQUIRE(rankNumber("12 und 3") == 12);
}

TEST_CASE("a name casts its highest rank in a German client", "[spell_rank]") {
    // The order a hash set hands them over in: rank 2 first. Before, every
    // "Rang" read as rank 0 and the first one won.
    const std::vector<Candidate> known = {
        {695, "Schattenblitz", "Rang 2"},   {686, "Schattenblitz", "Rang 1"},
        {47809, "Schattenblitz", "Rang 13"}, {27209, "Schattenblitz", "Rang 11"},
        {1454, "Aderlass", "Rang 1"},
    };
    REQUIRE(spellByName(known, "Schattenblitz") == 47809);
    REQUIRE(spellByName(known, "schattenblitz") == 47809);
    REQUIRE(spellByName(known, "!Schattenblitz") == 47809);
    REQUIRE(spellByName(known, "Aderlass") == 1454);
    REQUIRE(spellByName(known, "Feuerbrand") == 0);
}

TEST_CASE("a name with its rank in brackets casts that rank", "[spell_rank]") {
    const std::vector<Candidate> known = {
        {695, "Schattenblitz", "Rang 2"}, {686, "Schattenblitz", "Rang 1"}, {47809, "Schattenblitz", "Rang 13"},
    };
    REQUIRE(spellByName(known, "Schattenblitz(Rang 2)") == 695);
    REQUIRE(spellByName(known, "Schattenblitz (Rang 1)") == 686);
    REQUIRE(spellByName(known, "Schattenblitz(Rang 9)") == 0);
}

TEST_CASE("spells of one name and no rank give the same answer every run", "[spell_rank]") {
    const std::vector<Candidate> a = {{47241, "Metamorphose", ""}, {59672, "Metamorphose", ""}};
    const std::vector<Candidate> b = {{59672, "Metamorphose", ""}, {47241, "Metamorphose", ""}};
    REQUIRE(spellByName(a, "Metamorphose") == spellByName(b, "Metamorphose"));
}

TEST_CASE("the book orders by name, then by the rank's number", "[spell_rank]") {
    REQUIRE(bookBefore("Blutpakt", "Rang 8", 0, "Feuerblitz", "Rang 1", 0));
    REQUIRE_FALSE(bookBefore("Feuerblitz", "Rang 1", 0, "Blutpakt", "Rang 8", 0));
    REQUIRE(bookBefore("Feuerblitz", "Rang 2", 0, "Feuerblitz", "Rang 10", 0));
    REQUIRE_FALSE(bookBefore("Feuerblitz", "Rang 10", 0, "Feuerblitz", "Rang 2", 0));
    REQUIRE(bookBefore("feuerblitz", "", 0, "Feuerschild", "", 0));
}
