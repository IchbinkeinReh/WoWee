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

// A long text is stepped through once: it took a minute for each of these.
TEST_CASE("a long text is laid out and edited in a moment", "[edit]") {
    std::string text;
    for (int i = 0; i < 5000; ++i) text += "zeile|n";
    text += std::string(100000, 'x');
    const auto measure = [](const std::string& piece) {
        size_t letters = 0;
        for (const auto& run : parseMarkup(piece)) letters += run.text.size();
        return 6.0f * static_cast<float>(letters);
    };
    const auto starts = editLines(text, 100.0f, measure);
    CHECK(starts.size() > 6000);
    CHECK(editLines(text, 0.0f, measure).size() == 1);
    CHECK(caretSnap(text, text.size() - 1) == text.size() - 1);
    std::string box;
    const size_t caret = insertIntoEditBox(box, 0, text, -1, 255, false);
    CHECK(caret == box.size());
    CHECK(visibleLength(box) == 255);
    CHECK(wordDeleteBackFrom(text, text.size()) == 0);
    bool moved = false;
    CHECK(editLineMove(text, starts, text.size(), -1, moved) < text.size());
    CHECK(moved);
}

// 0x00966710, 0x009649d0: a box of one line shows what the caret is in.
TEST_CASE("a line longer than its box goes along with the caret", "[edit_lines]") {
    const std::string s = "abcdefghijklmnopqrstuvwxyz";
    const auto is = [](const EditShown& e, size_t first, size_t end) { return e.first == first && e.end == end; };
    CHECK(is(editShown(s, 0, 4, 8.0f, letters), 0, 8));
    CHECK(is(editShown(s, 0, 8, 8.0f, letters), 0, 8));
    // Past the end: three quarters of the width are before the caret.
    CHECK(is(editShown(s, 0, 9, 8.0f, letters), 3, 11));
    CHECK(is(editShown(s, 0, 26, 8.0f, letters), 20, 26));
    // Within what is shown it stays, before it a quarter is left of the caret.
    CHECK(is(editShown(s, 20, 21, 8.0f, letters), 20, 26));
    CHECK(is(editShown(s, 20, 10, 8.0f, letters), 8, 16));
    CHECK(is(editShown(s, 20, 0, 8.0f, letters), 0, 8));
    CHECK(is(editShown("ab", 20, 2, 8.0f, letters), 0, 2));
    CHECK(is(editShown(s, 20, 2, 0.0f, letters), 0, 26));
    // A link is one letter to it.
    const std::string link = "abcd|cffffffff|Hitem:1|h[xyz]|h|refgh";
    CHECK(is(editShown(link, 0, link.size(), 8.0f, letters), link.size() - 4, link.size()));

    CHECK(editOpenColour(link, 3).empty());
    CHECK(editOpenColour(link, 20) == "|cffffffff");
    CHECK(editOpenColour(link, link.size() - 1).empty());
    CHECK(editOpenColour("a||cffffffffb", 12).empty());
}
