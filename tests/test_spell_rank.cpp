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

// 0x00542030: a spell hidden from the book (Attributes 0x80) or a recipe
// (0x20) never reaches the name list; AttributesEx4 0x8000 keeps a spell
// out of the book but not out of the name list.
TEST_CASE("hidden and recipe spells are not in the name list or the book", "[spell_rank]") {
    REQUIRE(inNameList(0x10u));                 // Metamorphose 47241
    REQUIRE_FALSE(inNameList(0x800180u));       // its talent 59672
    REQUIRE_FALSE(inNameList(0x20u));           // a recipe
    REQUIRE(inPlayerBook(0x10u, 0));
    REQUIRE_FALSE(inPlayerBook(0x10u, 0x8000u));
    REQUIRE_FALSE(inPlayerBook(0x800180u, 0));
}

// Locki's two "Metamorphose": with the talent's teaching spell left out of
// the name list, the name is the form - not 59672, which the server answers
// with "Ihr beherrscht bereits Metamorphose".
TEST_CASE("Metamorphose by name is the form, not the talent", "[spell_rank]") {
    struct Row { uint32_t id; uint32_t attr; };
    const Row rows[] = {{59672, 0x800180u}, {47241, 0x10u}};
    std::vector<Candidate> known;
    for (const Row& r : rows)
        if (inNameList(r.attr)) known.push_back({r.id, "Metamorphose", ""});
    REQUIRE(spellByName(known, "Metamorphose") == 47241);
}

// 0x00541df0 files a rank with no number by its SpellLevel.
TEST_CASE("a rank with no number is placed by its spell level", "[spell_rank]") {
    const std::vector<Candidate> known = {{100, "Teleport", "", 20}, {200, "Teleport", "", 60}, {150, "Teleport", "", 40}};
    REQUIRE(spellByName(known, "Teleport") == 200);
    REQUIRE(bookBefore("Teleport", "", 20, "Teleport", "", 60));
    REQUIRE_FALSE(bookBefore("Teleport", "", 60, "Teleport", "", 20));
}

// 0x0053ca70: the highest-rank numbering keeps the last of each run of
// ranks, and every spell with no rank number.
TEST_CASE("the highest-rank numbering keeps the last rank of each spell", "[spell_rank]") {
    const std::vector<BookEntry> tab = {
        {"Aderlass", "Rang 1"}, {"Aderlass", "Rang 2"}, {"Aderlass", "Rang 8"},
        {"Ansteckung", "Rang 5"},
        {"Chaosblitz", ""}, {"Chaosblitz", ""},
        {"Fluch", ""}, {"Fluch", "Rang 2"},
    };
    REQUIRE(highestRankSlots(tab) == std::vector<size_t>{2, 3, 4, 5, 7});
    REQUIRE(highestRankSlots({}).empty());
}
