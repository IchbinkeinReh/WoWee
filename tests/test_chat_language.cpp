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

TEST_CASE("zhCN and zhTW cut a long word into pieces of 2, 3, 4, 5", "[chat_language]") {
    // Comprehension 0 replaces every piece, and a lookup answering each
    // length with that many x's shows where the cuts fell.
    std::map<uint32_t, std::vector<std::string>> echo;
    for (uint32_t n = 1; n <= 18; ++n) echo[n] = {std::string(n, 'x')};
    const auto byLength = [&](uint32_t n) -> const std::vector<std::string>* {
        auto it = echo.find(n);
        return it == echo.end() ? nullptr : &it->second;
    };
    const std::string out = garble("abcdefghijklmn", 0, byLength, 4);
    // 14 letters: 2 + 3 + 4 + 5, each cut with a space before the piece.
    CHECK(out == " xx xxx xxxx xxxxx");
    // enUS does not cut.
    CHECK(garble("abcdefghijklmn", 0, byLength, 0) == "xxxxxxxxxxxxxx");
}

TEST_CASE("koKR copies the case byte by byte", "[chat_language]") {
    std::map<uint32_t, std::vector<std::string>> one = {{3, {"abc"}}};
    const auto look = [&](uint32_t n) -> const std::vector<std::string>* {
        auto it = one.find(n);
        return it == one.end() ? nullptr : &it->second;
    };
    // An upper-case original letter makes the byte upper case; a lower-case
    // one leaves it as it is (no lower-casing, unlike enUS).
    CHECK(garble("XyZ", 0, look, 1) == "AbC");
}
