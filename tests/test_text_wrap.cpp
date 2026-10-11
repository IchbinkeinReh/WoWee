// Word wrapping for FontStrings.
//
// Pure arithmetic over strings and widths, which is why it is testable at all:
// the drawing side hands in an ImFont and here a character is one unit wide, so
// a width of ten means ten characters.
#include "catch_amalgamated.hpp"
#include "ui/text_wrap.hpp"

#include <string>
#include <vector>

using wowee::ui::WrapRun;
using wowee::ui::wrapText;

namespace {

/// One unit per character, so the numbers in these tests read as counts.
float charWidth(const std::string& s) { return static_cast<float>(s.size()); }

std::vector<WrapRun> plain(const std::string& text) {
    WrapRun r;
    r.text = text;
    return {r};
}

/// A line joined back into one string, which is what a reader wants to check.
std::string joined(const std::vector<WrapRun>& line) {
    std::string out;
    for (const WrapRun& r : line) out += r.text;
    return out;
}

std::vector<std::string> lines(const std::string& text, float width,
                               bool nonSpaceWrap = false) {
    std::vector<std::string> out;
    for (const auto& line : wrapText(plain(text), width, nonSpaceWrap, charWidth)) {
        out.push_back(joined(line));
    }
    return out;
}

}  // namespace

TEST_CASE("a width of zero means no wrapping at all", "[text_wrap]") {
    // What an auto-sized label wants: it is as wide as its own text, so there
    // is nothing to break it to.
    const auto out = lines("the quick brown fox jumps", 0.0f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] == "the quick brown fox jumps");
}

TEST_CASE("text shorter than the box stays on one line", "[text_wrap]") {
    const auto out = lines("short", 40.0f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] == "short");
}

TEST_CASE("a break falls between words", "[text_wrap]") {
    // "the quick " is ten and would be eleven with "brown", so the break comes
    // before it rather than inside it.
    const auto out = lines("the quick brown fox", 12.0f);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == "the quick ");
    REQUIRE(out[1] == "brown fox");
}

TEST_CASE("the space a break replaces does not start the next line",
          "[text_wrap]") {
    for (const std::string& line : lines("alpha beta gamma delta", 11.0f)) {
        REQUIRE(!line.empty());
        REQUIRE(line.front() != ' ');
    }
}

TEST_CASE("a word wider than the box stands alone by default", "[text_wrap]") {
    // Without nonspacewrap there is nowhere to break it, so it overflows on a
    // line of its own rather than being cut.
    const auto out = lines("hi extraordinarily ok", 6.0f);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == "hi ");
    REQUIRE(out[1] == "extraordinarily ");
    REQUIRE(out[2] == "ok");
}

TEST_CASE("nonspacewrap breaks inside a word too long to fit", "[text_wrap]") {
    const auto out = lines("abcdefghij", 4.0f, /*nonSpaceWrap=*/true);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == "abcd");
    REQUIRE(out[1] == "efgh");
    REQUIRE(out[2] == "ij");
    // Nothing is dropped on the way.
    std::string all;
    for (const auto& l : out) all += l;
    REQUIRE(all == "abcdefghij");
}

TEST_CASE("no text is lost across a wrap", "[text_wrap]") {
    const std::string text =
        "You have been chosen to carry word of this to the King himself.";
    for (float width : {8.0f, 13.0f, 21.0f, 34.0f}) {
        std::string all;
        for (const auto& l : lines(text, width)) all += l;
        // The only thing a break removes is the space it stands in for.
        std::string stripped;
        for (char c : text) if (c != ' ') stripped += c;
        std::string got;
        for (char c : all) if (c != ' ') got += c;
        REQUIRE(got == stripped);
    }
}

TEST_CASE("a colour run split across lines keeps its colour on both",
          "[text_wrap]") {
    // This is why the wrap works on runs rather than on the stripped string.
    WrapRun red;
    red.text = "alpha beta gamma";
    red.hasColor = true;
    red.rgba[0] = 1.0f; red.rgba[1] = 0.0f; red.rgba[2] = 0.0f; red.rgba[3] = 1.0f;

    const auto out = wrapText({red}, 12.0f, false, charWidth);
    REQUIRE(out.size() == 2);
    for (const auto& line : out) {
        REQUIRE(!line.empty());
        for (const WrapRun& r : line) {
            REQUIRE(r.hasColor);
            REQUIRE(r.rgba[0] == 1.0f);
            REQUIRE(r.rgba[1] == 0.0f);
        }
    }
}

TEST_CASE("adjacent pieces of one style merge back into a single run",
          "[text_wrap]") {
    // Otherwise a line of five words is five runs, and every one of them is a
    // separate draw call for no reason.
    const auto out = wrapText(plain("a b c d e"), 40.0f, false, charWidth);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].size() == 1);
    REQUIRE(out[0][0].text == "a b c d e");
}

TEST_CASE("a break between two runs does not merge their styles",
          "[text_wrap]") {
    WrapRun plainRun;
    plainRun.text = "white ";
    WrapRun colored;
    colored.text = "red";
    colored.hasColor = true;
    colored.rgba[0] = 1.0f; colored.rgba[1] = 0.0f; colored.rgba[2] = 0.0f;

    const auto out = wrapText({plainRun, colored}, 40.0f, false, charWidth);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].size() == 2);
    REQUIRE(out[0][0].text == "white ");
    REQUIRE(!out[0][0].hasColor);
    REQUIRE(out[0][1].text == "red");
    REQUIRE(out[0][1].hasColor);
}

TEST_CASE("a newline breaks the line even with no wrapping", "[text_wrap]") {
    // |n is WoW's spelling of one and the markup parser turns it into this, so
    // a label with an explicit break gets one whether or not anything wraps.
    const auto out = lines("first\nsecond", 0.0f);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == "first");
    REQUIRE(out[1] == "second");
}

TEST_CASE("two newlines leave a blank line between", "[text_wrap]") {
    // INSTANCE_LOCK_SEPARATOR is "%s|n|n%s", which is a paragraph break.
    const auto out = lines("a\n\nb", 0.0f);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == "a");
    REQUIRE(out[1].empty());
    REQUIRE(out[2] == "b");
}

TEST_CASE("a hard break wins over a soft one", "[text_wrap]") {
    // The word before the break must not be carried onto the wrapped line.
    const auto out = lines("ab\ncd ef gh", 5.0f);
    REQUIRE(out.size() >= 2);
    REQUIRE(out[0] == "ab");
    REQUIRE(out[1].substr(0, 2) == "cd");
}

TEST_CASE("a word is not carried across a hard break", "[text_wrap]") {
    const auto out = lines("alpha\nbeta", 40.0f);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == "alpha");
    REQUIRE(out[1] == "beta");
}

TEST_CASE("an empty string produces one empty line", "[text_wrap]") {
    const auto out = wrapText(plain(""), 10.0f, false, charWidth);
    REQUIRE(out.size() == 1);
    REQUIRE(joined(out[0]).empty());
}

// ---------------------------------------------------------------------------
// A label too long for its box is cut at the end with "...", as WoW cuts it.
// Before this it was justified as if it fitted: a right-aligned one started
// out past the left of its box and lost its first characters to the clip, so
// the video options' resolution read "920x1080 (Wide)".

using wowee::ui::fitWithEllipsis;

TEST_CASE("text that fits is left alone", "[wrap][ellipsis]") {
    CHECK(fitWithEllipsis("1920x1080", 9.0f, charWidth) == "1920x1080");
    CHECK(fitWithEllipsis("", 0.0f, charWidth).empty());
}

TEST_CASE("text that does not fit keeps its start and loses its end", "[wrap][ellipsis]") {
    // Sixteen characters in fifteen: the dots take three, so twelve remain.
    const std::string cut = fitWithEllipsis("1920x1080 (Wide)", 15.0f, charWidth);
    CHECK(cut == "1920x1080 (W...");
    CHECK(charWidth(cut) <= 15.0f);
    CHECK(cut.rfind("1920", 0) == 0);
}

TEST_CASE("no space is left hanging before the dots", "[wrap][ellipsis]") {
    // "Long " would be cut to fit six, and the space before the dots dropped.
    CHECK(fitWithEllipsis("Long quest name", 8.0f, charWidth) == "Long...");
}

TEST_CASE("a character is never cut in half", "[wrap][ellipsis]") {
    // "é" is two bytes. Measured in bytes here, so the cut has to back up over
    // the continuation byte rather than land between the two.
    const std::string cut = fitWithEllipsis("caf\xC3\xA9 au lait", 7.0f, charWidth);
    CHECK(cut == "caf...");
    const std::string whole = fitWithEllipsis("caf\xC3\xA9 au lait", 8.0f, charWidth);
    CHECK(whole == "caf\xC3\xA9...");
}

TEST_CASE("a box too small for the dots gets the dots", "[wrap][ellipsis]") {
    CHECK(fitWithEllipsis("Anything", 2.0f, charWidth) == "...");
}

// ---------------------------------------------------------------------------
// Where a line starts in its box. A message frame's lines were drawn with no
// justify at all, so UIErrorsFrame (512 wide, justifyH="CENTER") started every
// error at its left edge: only a line nearly as wide as the frame looked
// centred, and "Not enough mana" sat well left of the middle of the screen.

using wowee::ui::justifyOffset;

TEST_CASE("a centred line sits in the middle of its box whatever its length", "[wrap][justify]") {
    // The middle of the line is the middle of the box, for a short line and a
    // long one alike - which is what made the old drawing look right only
    // sometimes.
    CHECK(justifyOffset("CENTER", 512.0f, 120.0f) + 120.0f * 0.5f == 256.0f);
    CHECK(justifyOffset("CENTER", 512.0f, 500.0f) + 500.0f * 0.5f == 256.0f);
    CHECK(justifyOffset("CENTER", 512.0f, 512.0f) == 0.0f);
}

TEST_CASE("each wrapped line of a centred message is centred on its own", "[wrap][justify]") {
    // Two lines of different widths, and each one's middle on the box's.
    const auto lines = wowee::ui::wrapText(
        {wowee::ui::WrapRun{"aaaaaaa bb"}}, 8.0f, false, charWidth);
    REQUIRE(lines.size() == 2);
    const float w0 = charWidth(lines[0][0].text);
    const float w1 = charWidth(lines[1][0].text);
    REQUIRE(w0 != w1);
    CHECK(justifyOffset("CENTER", 8.0f, w0) + w0 * 0.5f == 4.0f);
    CHECK(justifyOffset("CENTER", 8.0f, w1) + w1 * 0.5f == 4.0f);
}

TEST_CASE("left, right, unknown and missing justify", "[wrap][justify]") {
    CHECK(justifyOffset("RIGHT", 512.0f, 112.0f) == 400.0f);
    CHECK(justifyOffset("LEFT", 512.0f, 112.0f) == 0.0f);
    CHECK(justifyOffset("JUSTIFY", 512.0f, 112.0f) == 0.0f);
    CHECK(justifyOffset(nullptr, 512.0f, 112.0f) == 0.0f);
}

TEST_CASE("a box with no width has nothing to justify in", "[wrap][justify]") {
    // An auto-sized label is as wide as its text and starts where it is put.
    CHECK(justifyOffset("CENTER", 0.0f, 112.0f) == 0.0f);
    CHECK(justifyOffset("RIGHT", 0.0f, 112.0f) == 0.0f);
}
