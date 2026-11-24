// The lines an edit box draws its text in (ui/edit_lines.hpp).
#include <catch_amalgamated.hpp>

#include "ui/edit_lines.hpp"

using namespace wowee::ui;

namespace {
// A letter is as wide as any other; markup has no width.
float letters(const std::string& piece) {
    float n = 0.0f;
    for (const auto& run : parseMarkup(piece)) n += static_cast<float>(run.text.size());
    return n;
}
std::vector<std::string> drawn(const std::string& s, float width) {
    const auto starts = editLines(s, width, letters);
    std::vector<std::string> out;
    for (size_t i = 0; i < starts.size(); ++i)
        out.push_back(s.substr(starts[i], editLineEnd(s, starts, i) - starts[i]));
    return out;
}
}  // namespace

TEST_CASE("an edit box's own breaks begin lines", "[edit_lines]") {
    CHECK(drawn("", 10.0f) == std::vector<std::string>{""});
    CHECK(drawn("/eins\n/zwei", 0.0f) == std::vector<std::string>{"/eins", "/zwei"});
    CHECK(drawn("a\n\nb\n", 10.0f) == std::vector<std::string>{"a", "", "b", ""});
    // Unwrapped, the width is nothing to it.
    CHECK(drawn("one line of it", 0.0f) == std::vector<std::string>{"one line of it"});
}

TEST_CASE("an edit box's words go to the next line where they do not fit", "[edit_lines]") {
    CHECK(drawn("one two three", 8.0f) == std::vector<std::string>{"one two ", "three"});
    // The spaces at a line's end are no reason for another.
    CHECK(drawn("one two    three", 7.0f) == std::vector<std::string>{"one two    ", "three"});
    CHECK(drawn("aaa bbb\nccc ddd", 4.0f) == std::vector<std::string>{"aaa ", "bbb", "ccc ", "ddd"});
    // A word longer than a line goes on in the next.
    CHECK(drawn("abcdefgh", 3.0f) == std::vector<std::string>{"abc", "def", "gh"});
    CHECK(drawn("ab cdefgh", 4.0f) == std::vector<std::string>{"ab ", "cdef", "gh"});
    // A link is as wide as what is seen of it.
    const std::string link = "|cffffffff|Hitem:1|h[Ab]|h|r";
    CHECK(drawn("xx " + link, 7.0f).size() == 1);
    CHECK(drawn("xx " + link, 6.0f).size() == 2);
    // No room for a letter is room for one.
    CHECK(drawn("abc", 0.5f) == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("a place in the text is on the line it starts", "[edit_lines]") {
    const std::string s = "one two three\nfour";
    const auto starts = editLines(s, 8.0f, letters);
    REQUIRE(starts == std::vector<size_t>{0, 8, 14});
    CHECK(editLineOf(starts, 0) == 0);
    CHECK(editLineOf(starts, 7) == 0);
    CHECK(editLineOf(starts, 8) == 1);
    CHECK(editLineOf(starts, 13) == 1);
    CHECK(editLineOf(starts, 14) == 2);
    CHECK(editLineOf(starts, 99) == 2);
    CHECK(editLineEnd(s, starts, 0) == 8);
    CHECK(editLineEnd(s, starts, 1) == 13);
    CHECK(editLineEnd(s, starts, 2) == s.size());
}

// 0x00964550: as far into the next line as into this one, never past its end;
// nothing above the first line or below the last.
TEST_CASE("Up and Down keep the column", "[edit_lines]") {
    const std::string s = "abcd\nx\nwxyz";
    const auto starts = editLines(s, 0.0f, letters);
    bool moved = false;
    CHECK(editLineMove(s, starts, 3, 1, moved) == 6);   // "abc|d" -> "x|"
    CHECK(moved);
    CHECK(editLineMove(s, starts, 6, 1, moved) == 8);   // "x|" -> "w|xyz"
    CHECK(editLineMove(s, starts, 10, -1, moved) == 6);
    CHECK(editLineMove(s, starts, 2, -1, moved) == 2);
    CHECK_FALSE(moved);
    CHECK(editLineMove(s, starts, 11, 1, moved) == 11);
    CHECK_FALSE(moved);
    // By the lines drawn, not the text's own.
    const std::string w = "one two three";
    const auto drawnLines = editLines(w, 8.0f, letters);
    CHECK(editLineMove(w, drawnLines, 2, 1, moved) == 10);
    CHECK(moved);
    CHECK(editLineMove(w, drawnLines, 10, -1, moved) == 2);
    // Never onto the start of the line after.
    const std::string l = "a b\nlonger line";
    const auto two = editLines(l, 4.0f, letters);
    for (size_t at = 0; at <= l.size(); ++at) {
        const size_t up = editLineMove(l, two, at, -1, moved);
        if (moved) CHECK(editLineOf(two, up) + 1 == editLineOf(two, at));
        const size_t down = editLineMove(l, two, at, 1, moved);
        if (moved) CHECK(editLineOf(two, down) == editLineOf(two, at) + 1);
    }
}

// 0x00482510.
TEST_CASE("the mouse finds the letters before it", "[edit_lines]") {
    const std::string s = "one two three\nfour";
    const auto starts = editLines(s, 8.0f, letters);
    CHECK(editPlaceIn(s, starts, 0, -3.0f, letters) == 0);
    CHECK(editPlaceIn(s, starts, 0, 2.5f, letters) == 2);
    CHECK(editPlaceIn(s, starts, 1, 2.0f, letters) == 10);
    CHECK(editPlaceIn(s, starts, 1, 100.0f, letters) == 13);
    CHECK(editPlaceIn(s, starts, 2, 100.0f, letters) == s.size());
    // Not past a wrapped line's last letter: that place is the next line's.
    CHECK(editLineOf(starts, editPlaceIn(s, starts, 0, 100.0f, letters)) == 0);
    CHECK(editPlaceIn(s, starts, 9, 1.0f, letters) == 15);
}
