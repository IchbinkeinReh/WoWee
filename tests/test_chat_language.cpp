#include <catch_amalgamated.hpp>

#include "game/chat_language.hpp"

#include <map>

using namespace wowee::game::chat_language;

// 0x00505930 and its helpers.

namespace {

std::map<uint32_t, std::vector<std::string>> kWords = {
    {1, {"a"}},
    {2, {"ko", "no"}},
    {3, {"lon", "ash", "nud"}},
    {5, {"thorg"}},
};

const std::vector<std::string>* lookup(uint32_t len) {
    auto it = kWords.find(len);
    return it == kWords.end() ? nullptr : &it->second;
}

}  // namespace

TEST_CASE("A language the listener speaks is heard as said", "[chat_language]") {
    CHECK(garble("Well met, friend!", 300, lookup) == "Well met, friend!");
}

TEST_CASE("Every word is replaced at no comprehension", "[chat_language]") {
    const std::string out = garble("Hey you", 0, lookup);
    // "Hey" (3) takes a three-letter word in its case; "you" likewise.
    REQUIRE(out.size() == 7);
    CHECK(out[3] == ' ');
    CHECK(out[0] >= 'A');
    CHECK(out[0] <= 'Z');
    CHECK(out.substr(1, 2) == lookup(3)->at(stringHash("Hey") % 3).substr(1, 2));
    CHECK(out.substr(4) == lookup(3)->at(stringHash("you") % 3));
}

TEST_CASE("The same word is always the same word", "[chat_language]") {
    CHECK(garble("friend", 0, lookup) == garble("friend", 0, lookup));
    // Case-insensitive hash: the pick does not depend on case, the case does.
    CHECK(stringHash("Friend") == stringHash("FRIEND"));
}

TEST_CASE("Separators collapse to one space and a missing length falls back", "[chat_language]") {
    // Four letters: no four-letter word, so a three-letter one, cut to fit.
    const std::string out = garble("wait... !go", 0, lookup);
    CHECK(out.size() == 3 + 1 + 2);
    CHECK(out[3] == ' ');
}

TEST_CASE("Links, colours and textures go before the words are read", "[chat_language]") {
    CHECK(stripEscapes("|cff1eff00|Hitem:123:0|h[Sword]|h|r") == "Sword");
    CHECK(stripEscapes("a |TInterface\\Icons\\x:0|t b") == "a  b");
    CHECK(stripEscapes("x||y") == "x||y");
}

TEST_CASE("The hash never answers zero", "[chat_language]") {
    CHECK(stringHash("") == 0x7fed7fedu);
    CHECK(stringHash("a/b") == stringHash("A\\B"));
}
