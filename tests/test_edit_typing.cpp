// What is typed into an edit box (ui/edit_typing.hpp).
#include <catch_amalgamated.hpp>

#include "ui/edit_typing.hpp"

#include <SDL3/SDL_keycode.h>

using namespace wowee::ui;

TEST_CASE("an edit box's keys are SDL's", "[edit_typing]") {
    CHECK(edit_key::kDelete == static_cast<int>(SDLK_DELETE));
    CHECK(edit_key::kBackspace == static_cast<int>(SDLK_BACKSPACE));
    CHECK(edit_key::kTab == static_cast<int>(SDLK_TAB));
    CHECK(edit_key::kReturn == static_cast<int>(SDLK_RETURN));
    CHECK(edit_key::kEscape == static_cast<int>(SDLK_ESCAPE));
    CHECK(edit_key::kInsert == static_cast<int>(SDLK_INSERT));
    CHECK(edit_key::kHome == static_cast<int>(SDLK_HOME));
    CHECK(edit_key::kEnd == static_cast<int>(SDLK_END));
    CHECK(edit_key::kLeft == static_cast<int>(SDLK_LEFT));
    CHECK(edit_key::kRight == static_cast<int>(SDLK_RIGHT));
    CHECK(edit_key::kUp == static_cast<int>(SDLK_UP));
    CHECK(edit_key::kDown == static_cast<int>(SDLK_DOWN));
}

// 0x00965ce0
TEST_CASE("typed and pasted text comes a character at a time", "[edit_typing]") {
    using Pieces = std::vector<std::string>;
    CHECK(typedPieces("ab c", false) == Pieces{"a", "b", " ", "c"});
    CHECK(typedPieces("\xc3\xa4\xe2\x82\xac", false) == Pieces{"\xc3\xa4", "\xe2\x82\xac"});
    CHECK(typedPieces("a|c", false) == Pieces{"a", "||", "c"});
    CHECK(typedPieces("a\tb", false) == Pieces{"a", "    ", "b"});
    CHECK(typedPieces("a\r\n\x01\x7f", false) == Pieces{"a"});
    CHECK(typedPieces("a\r\n", true) == Pieces{"a", "\n"});
    CHECK(typedPieces("\xc3", false) == Pieces{"\xc3"});
}

// 0x00965960
TEST_CASE("a numeric box takes digits alone", "[edit_typing]") {
    CHECK_FALSE(numericRefuses("12"));
    CHECK(numericRefuses("1a"));
    CHECK(numericRefuses("    "));
}
