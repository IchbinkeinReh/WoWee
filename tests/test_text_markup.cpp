// The inline markup a label carries: colour, links, line breaks, textures.
//
// The case that made this a file of its own: |Hitem:3299|h[Fractured Canine]|h
// drew as nothing at all. The parser skipped from the opening marker to the
// next bar - which is the |h *before* the display text - and then skipped
// again from there to the |h after it, so the name between them went with the
// payload. "You receive loot: [Fractured Canine]." rendered as "You receive
// loot: ." on every FrameXML surface that draws a link, which is the loot
// stream, quest rewards, achievement text and every tooltip carrying one.
//
// It went unnoticed because the function lived inside widget_renderer.cpp with
// ImGui, where nothing could reach it.
#include "catch_amalgamated.hpp"
#include "ui/text_markup.hpp"

#include <string>

using wowee::ui::parseMarkup;
using wowee::ui::WrapRun;
using wowee::ui::caretStepLeft;
using wowee::ui::caretStepRight;
using wowee::ui::caretSnap;

namespace {
/// Everything the runs would draw, which is what the reader sees.
std::string drawn(const std::string& in) {
    std::string out;
    for (const WrapRun& r : parseMarkup(in)) out += r.text;
    return out;
}
}  // namespace

TEST_CASE("a link keeps its display text", "[markup]") {
    SECTION("the name between the markers survives") {
        REQUIRE(drawn("|Hitem:6948:0:0:0:0:0:0:0|h[Hearthstone]|h") == "[Hearthstone]");
    }

    SECTION("and the text around it") {
        REQUIRE(drawn("You receive loot: |cff9d9d9d|Hitem:3299|h[Fractured Canine]|h|r.")
                == "You receive loot: [Fractured Canine].");
    }

    SECTION("two links in one line stay separate runs") {
        const auto runs = parseMarkup("|Hitem:1|h[A]|h and |Hspell:2|h[B]|h");
        std::string first, second;
        for (const WrapRun& r : runs) {
            if (r.link == "item:1")  first  += r.text;
            if (r.link == "spell:2") second += r.text;
        }
        REQUIRE(first == "[A]");
        REQUIRE(second == "[B]");
    }

    SECTION("the payload is carried, so a click can name what it hit") {
        const auto runs = parseMarkup("|Hquest:1234:60|h[Kill Ten Rats]|h");
        bool found = false;
        for (const WrapRun& r : runs) {
            if (r.text == "[Kill Ten Rats]") {
                REQUIRE(r.link == "quest:1234:60");
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("text after the closing marker is not part of the link") {
        for (const WrapRun& r : parseMarkup("|Hitem:1|h[A]|h tail")) {
            if (r.text.find("tail") != std::string::npos) REQUIRE(r.link.empty());
        }
    }
}

TEST_CASE("the other escapes still behave", "[markup]") {
    SECTION("a colour escape sets the run's colour and draws nothing itself") {
        const auto runs = parseMarkup("|cffff0000red|r");
        REQUIRE(drawn("|cffff0000red|r") == "red");
        REQUIRE(runs.front().hasColor);
    }
    SECTION("|n is a line break") { REQUIRE(drawn("a|nb") == "a\nb"); }
    SECTION("|| is a literal bar") { REQUIRE(drawn("a||b") == "a|b"); }
    SECTION("an inline texture contributes no letters") {
        REQUIRE(drawn("a|TInterface\\Icons\\X:16|tb") == "ab");
    }
    SECTION("an unterminated link does not run off the end") {
        REQUIRE(drawn("|Hitem:1") == "");
    }
}

// ── Inline textures ─────────────────────────────────────────────────────────
//
// A price is written as a number followed by a picture of a coin:
// GOLD_AMOUNT_TEXTURE is "%d|TInterface\MoneyFrame\UI-GoldIcon:%d:%d:2:0|t".
// The escape used to be skipped over, so every amount in the interface came
// out as bare digits with nothing to say which coin it counted - a tooltip's
// sell price read "19 81 56".

TEST_CASE("an inline texture becomes a run that names its file", "[markup]") {
    const auto runs = parseMarkup("19|TInterface\\MoneyFrame\\UI-GoldIcon:0:0:2:0|t");
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].text == "19");
    CHECK(runs[0].texture.empty());
    CHECK(runs[1].texture == "Interface\\MoneyFrame\\UI-GoldIcon");
    CHECK(runs[1].text.empty());
}

TEST_CASE("the size fields are read, and zero means the line's height",
          "[markup]") {
    const auto sized = parseMarkup("|TInterface\\Icons\\X:16:12:2:0|t");
    REQUIRE(sized.size() == 1);
    CHECK(sized[0].texHeight == 16.0f);
    CHECK(sized[0].texWidth == 12.0f);

    // What every coin escape sends: zero for both.
    const auto coin = parseMarkup("|TInterface\\MoneyFrame\\UI-GoldIcon:0:0:2:0|t");
    REQUIRE(coin.size() == 1);
    CHECK(coin[0].texHeight == 0.0f);
    CHECK(coin[0].texWidth == 0.0f);
}

TEST_CASE("a price keeps a coin beside each amount", "[markup]") {
    // The whole string GetCoinTextureString builds for 19g 81s 56c.
    const std::string price =
        "19|TInterface\\MoneyFrame\\UI-GoldIcon:0:0:2:0|t "
        "81|TInterface\\MoneyFrame\\UI-SilverIcon:0:0:2:0|t "
        "56|TInterface\\MoneyFrame\\UI-CopperIcon:0:0:2:0|t";
    const auto runs = parseMarkup(price);

    int coins = 0;
    std::string letters;
    for (const auto& r : runs) {
        if (!r.texture.empty()) ++coins;
        letters += r.text;
    }
    CHECK(coins == 3);
    CHECK(letters == "19 81 56");
    // And no letter 'g', 's' or 'c' anywhere: the picture is what says which
    // coin it is, exactly as the real client writes it.
    CHECK(letters.find('g') == std::string::npos);
}

TEST_CASE("a texture with no path is not a run", "[markup]") {
    CHECK(parseMarkup("|T|t").empty());
    CHECK(parseMarkup("a|T|tb").size() == 1);
}

TEST_CASE("an unterminated texture does not run off the end", "[markup]") {
    const auto runs = parseMarkup("a|TInterface\\Icons\\X");
    // The 'a' and the texture, and nothing past the end of the string.
    CHECK(runs.size() == 2);
    CHECK(runs[1].texture == "Interface\\Icons\\X");
}

TEST_CASE("a stray closing marker draws nothing", "[markup]") {
    CHECK(drawn("a|tb") == "ab");
}

TEST_CASE("a texture keeps the colour it sits inside", "[markup]") {
    // A coin inside a coloured span is still that span's run, so the icon is
    // tinted the way the surrounding text is.
    const auto runs = parseMarkup("|cffff0000x|TInterface\\Icons\\X:0:0|ty|r");
    REQUIRE(runs.size() == 3);
    CHECK(runs[1].texture == "Interface\\Icons\\X");
    CHECK(runs[1].hasColor);
    CHECK(runs[1].rgba[0] == 1.0f);
}


// ── The caret walks what is drawn ───────────────────────────────────────────
//
// Only reachable since links became clickable: shift-clicking one puts the
// whole "|Hitem:3299|h[Fractured Canine]|h" into the edit box, and the box
// draws its display text. A caret stepping one byte at a time would sit still
// for forty keypresses crossing the payload and then jump a word.
TEST_CASE("the caret steps by drawn characters", "[markup]") {
    const std::string line = "hi |Hitem:1|h[AB]|h x";

    SECTION("right from the start crosses the plain text one at a time") {
        REQUIRE(caretStepRight(line, 0) == 1);
        REQUIRE(caretStepRight(line, 1) == 2);
    }

    SECTION("a link is one step whole, so the caret never rests inside it") {
        // "hi " is three characters and the link runs from 3 to the end. One
        // step from 3 clears all of it - payload, display text and closing
        // marker - because a caret inside a link is a caret that can leave
        // half an escape behind when something is erased.
        REQUIRE(caretStepRight(line, 3) == 19);
        REQUIRE(line.substr(19) == " x");
    }

    SECTION("left is the inverse of right, everywhere along the line") {
        for (size_t p = 0; p < line.size();) {
            const size_t next = caretStepRight(line, p);
            if (next >= line.size()) break;
            REQUIRE(caretStepLeft(line, next) == p);
            p = next;
        }
    }

    SECTION("neither runs past an end") {
        REQUIRE(caretStepRight(line, line.size()) == line.size());
        REQUIRE(caretStepLeft(line, 0) == 0);
    }

    SECTION("a colour escape draws nothing, so the caret does not stop in it") {
        // "a|cffff0000b|rc": the escape is ten bytes drawing nothing, so a
        // step from after 'a' crosses it and 'b' together.
        const std::string coloured = "a|cffff0000b|rc";
        REQUIRE(coloured.substr(11, 1) == "b");
        REQUIRE(caretStepRight(coloured, 1) == 14);  // its end with it (0x00962d50)
    }
}


// ── Erasing uses the same step, so the two cannot disagree ─────────────────
//
// Backspace over a link has to take the whole link. Removing a byte leaves
// half an escape behind, which the parser then reads as whatever the wreckage
// resembles - and the text is the player's own message, so the damage is
// visible and unrecoverable by pressing the key again.
namespace {

/// Backspace, as the edit box performs it: erase back to the previous caret
/// position.
std::string backspaceAt(std::string s, size_t& at) {
    const size_t from = caretStepLeft(s, at);
    s.erase(from, at - from);
    at = from;
    return s;
}

}  // namespace

TEST_CASE("erasing removes what draws as one character", "[markup]") {
    SECTION("backspace over a link takes all of it") {
        std::string s = "hi |Hitem:1|h[AB]|h";
        size_t at = s.size();
        // One press: the link is a single unit.
        s = backspaceAt(s, at);
        REQUIRE(s == "hi ");
        REQUIRE(at == 3);
    }

    SECTION("and never leaves half an escape") {
        std::string s = "a|cffff0000b|rc";
        size_t at = s.size();
        while (at > 0) {
            s = backspaceAt(s, at);
            // Whatever is left must still parse to something sane: every bar
            // in it is either doubled or opens an escape that closes.
            REQUIRE(parseMarkup(s).size() <= 3);
        }
        REQUIRE(s.empty());
    }
}


TEST_CASE("a position from outside is snapped to one the caret can occupy", "[markup]") {
    const std::string line = "hi |Hitem:1|h[AB]|h x";

    SECTION("a position inside a link comes back to its start") {
        // Anywhere from the opening bar to the last byte of the closing marker
        // is inside the link, and all of it snaps to where the link begins.
        for (size_t at = 4; at < 19; ++at) REQUIRE(caretSnap(line, at) == 3);
    }

    SECTION("positions outside one are left alone") {
        REQUIRE(caretSnap(line, 0) == 0);
        REQUIRE(caretSnap(line, 3) == 3);
        REQUIRE(caretSnap(line, 19) == 19);
        REQUIRE(caretSnap(line, line.size()) == line.size());
    }

    SECTION("every snapped position is one stepping can reach") {
        for (size_t at = 0; at <= line.size(); ++at) {
            const size_t snapped = caretSnap(line, at);
            bool reachable = (snapped == 0);
            for (size_t p = 0; !reachable && p < line.size();) {
                const size_t next = caretStepRight(line, p);
                if (next <= p) break;
                if (next == snapped) reachable = true;
                p = next;
            }
            REQUIRE(reachable);
        }
    }
}

// Replacing a selected run, which is what makes autocomplete usable.
//
// The interface writes the completed name and highlights the part it added, so
// the next character typed replaces the completion instead of landing after
// it. Everything about that is byte offsets, and both ends are easy to get
// wrong in ways nobody sees until a name comes out doubled.
TEST_CASE("A selected run is replaced, and only that run", "[edit][selection]") {
    using wowee::ui::EditSelection;
    using wowee::ui::replaceSelection;

    SECTION("the completion is taken and the typed prefix kept") {
        // "Thr" was typed, autocomplete wrote "Thrall" and highlighted "all".
        std::string t = "Thrall";
        const size_t c = replaceSelection(t, 6, {true, 3, 6});
        CHECK(t == "Thr");
        CHECK(c == 3);
    }
    SECTION("a run in the middle leaves both sides") {
        std::string t = "abcdef";
        const size_t c = replaceSelection(t, 6, {true, 2, 4});
        CHECK(t == "abef");
        CHECK(c == 2);
    }
    SECTION("all of it") {
        std::string t = "Thrall";
        const size_t c = replaceSelection(t, 6, {true, 0, 6});
        CHECK(t.empty());
        CHECK(c == 0);
    }

    // The three shapes that must do nothing. FrameXML clears a highlight by
    // passing equal offsets, and a zero-width run that still counted would eat
    // the next character typed - somewhere far from whatever set it.
    SECTION("no selection changes nothing") {
        std::string t = "Thrall";
        CHECK(replaceSelection(t, 4, {false, 0, 6}) == 4);
        CHECK(t == "Thrall");
    }
    SECTION("an empty range changes nothing") {
        std::string t = "Thrall";
        CHECK(replaceSelection(t, 4, {true, 3, 3}) == 4);
        CHECK(t == "Thrall");
    }
    SECTION("an inverted range changes nothing") {
        std::string t = "Thrall";
        CHECK(replaceSelection(t, 4, {true, 5, 2}) == 4);
        CHECK(t == "Thrall");
    }
    SECTION("a range past the end changes nothing") {
        // Rather than clamping: a stale range means the text moved under the
        // selection, and erasing some other run is worse than erasing none.
        std::string t = "Thr";
        CHECK(replaceSelection(t, 3, {true, 1, 99}) == 3);
        CHECK(t == "Thr");
    }
}

// What a letter limit counts.
//
// An edit box's limit is in characters shown, not bytes held, unless the box
// asked for countInvisibleLetters. The chat box does not ask - it declares
// letters="255" and nothing else - so the escapes a shift-clicked item link
// brings with it must not be charged against it.
TEST_CASE("A letter limit counts what is shown, not what is stored",
          "[edit][markup]") {
    using wowee::ui::visibleLength;

    SECTION("plain text is its own length") {
        CHECK(visibleLength("hello") == 5);
        CHECK(visibleLength("") == 0);
    }
    SECTION("a colour escape shows nothing") {
        // Ten bytes of escape around four shown.
        CHECK(visibleLength("|cffff0000fire|r") == 4);
    }
    SECTION("an item link shows only its name") {
        const std::string link =
            "|cff9d9d9d|Hitem:3299:0:0:0:0:0:0:0|h[Fractured Canine]|h|r";
        CHECK(visibleLength(link) == std::string("[Fractured Canine]").size());
        // And the whole point: the stored form is far longer, so charging
        // bytes against a 255 limit would let three links fill a message WoW
        // would have carried a dozen in.
        CHECK(link.size() > visibleLength(link) * 3);
    }
    SECTION("a message with a link keeps its own words") {
        const std::string msg =
            "look at |cff9d9d9d|Hitem:3299|h[Fractured Canine]|h|r now";
        CHECK(visibleLength(msg) ==
              std::string("look at [Fractured Canine] now").size());
    }
}

// SecondsToTime's "%d |4Sec:Sec;" drew raw in the battleground queue tooltip:
// "Time in queue: 13 |4Sec:Sec;".
TEST_CASE("the plural escape picks its word by the number before it", "[markup]") {
    REQUIRE(drawn("13 |4Sec:Sec;") == "13 Sec");
    REQUIRE(drawn("1 |4Min:Mins;") == "1 Min");
    REQUIRE(drawn("2 |4Min:Mins; 5 |4Sec:Sec;") == "2 Mins 5 Sec");
    REQUIRE(drawn("11 |4hour:hours;") == "11 hours");
    REQUIRE(drawn("|4one:many;") == "many");
}

// Ctrl+Left and Ctrl+Right in an edit box step a word; a link is one word's
// worth of step, never entered.
TEST_CASE("the caret steps a word at a time with Ctrl", "[markup][edit]") {
    using wowee::ui::caretWordLeft;
    using wowee::ui::caretWordRight;
    const std::string line = "say hello  there";
    CHECK(caretWordRight(line, 0) == 4);
    CHECK(caretWordRight(line, 4) == 11);
    CHECK(caretWordRight(line, 11) == line.size());
    CHECK(caretWordRight(line, line.size()) == line.size());
    CHECK(caretWordLeft(line, line.size()) == 11);
    CHECK(caretWordLeft(line, 11) == 4);
    CHECK(caretWordLeft(line, 6) == 4);
    CHECK(caretWordLeft(line, 4) == 0);
    CHECK(caretWordLeft(line, 0) == 0);

    const std::string link = "|Hitem:3299|h[Fractured Canine]|h";
    const std::string withLink = "see " + link + " now";
    // The space inside the link's name is not a word break: the link is one step.
    CHECK(caretWordRight(withLink, 4) == 4 + link.size() + 1);
    CHECK(caretWordLeft(withLink, 4 + link.size()) == 4);
}

// Shift with a caret key carries the highlight from where it was anchored.
TEST_CASE("Shift extends the selection from its anchor", "[markup][edit]") {
    using wowee::ui::EditSelection;
    using wowee::ui::moveCaret;
    // Nothing selected: the caret is the anchor.
    auto m = moveCaret(5, EditSelection{}, 6, true);
    CHECK(m.caret == 6);
    CHECK(m.sel.active);
    CHECK(m.sel.start == 5);
    CHECK(m.sel.stop == 6);
    // Further right, the same anchor.
    m = moveCaret(6, m.sel, 8, true);
    CHECK(m.sel.start == 5);
    CHECK(m.sel.stop == 8);
    // Back across the anchor: the run flips to the other side, still in order.
    m = moveCaret(8, m.sel, 3, true);
    CHECK(m.caret == 3);
    CHECK(m.sel.start == 3);
    CHECK(m.sel.stop == 5);
    // Back onto the anchor: nothing left highlighted.
    m = moveCaret(3, m.sel, 5, true);
    CHECK_FALSE(m.sel.active);
    // Without Shift the highlight is let go.
    m = moveCaret(5, EditSelection{true, 2, 5}, 4, false);
    CHECK(m.caret == 4);
    CHECK_FALSE(m.sel.active);
    // Ctrl+A leaves the caret at the end, anchored at the start: Shift+Left
    // gives back the last character.
    m = moveCaret(10, EditSelection{true, 0, 10}, 9, true);
    CHECK(m.sel.start == 0);
    CHECK(m.sel.stop == 9);
}

TEST_CASE("Home and End stop at the caret's own line", "[text_markup][editbox]") {
    // 0x00964200 / 0x009642e0.
    const std::string t = "ab\ncde\nf";
    CHECK(wowee::ui::caretLineStart(t, 5) == 3);
    CHECK(wowee::ui::caretLineEnd(t, 4) == 6);
    CHECK(wowee::ui::caretLineStart(t, 1) == 0);
    CHECK(wowee::ui::caretLineEnd(t, 7) == 8);
}

TEST_CASE("Word deletes take the spaces and one word", "[text_markup][editbox]") {
    // 0x00963b60 back, 0x00963a10 forward.
    const std::string t = "one two  three";
    CHECK(wowee::ui::wordDeleteBackFrom(t, 9) == 4);     // "one |two  |three"
    CHECK(wowee::ui::wordDeleteBackFrom(t, 2) == 0);
    CHECK(wowee::ui::wordDeleteForwardTo(t, 7) == 14);   // "two|  three|"
    CHECK(wowee::ui::wordDeleteForwardTo(t, 0) == 3);
}

// The chat box declares ignoreArrows: the character turns while a line is
// typed, and the box has the arrows with Alt (0x00967760).
TEST_CASE("an edit box may leave the arrows to the game", "[text_markup]") {
    using wowee::ui::isArrowKey;
    using wowee::ui::leavesArrows;
    for (const int key : {0x4000004F, 0x40000050, 0x40000051, 0x40000052}) CHECK(isArrowKey(key));
    for (const int key : {0x4000004A, 0x4000004D, 0x40000053, int('a'), 13, 8}) CHECK_FALSE(isArrowKey(key));
    CHECK(leavesArrows(true, false));
    CHECK_FALSE(leavesArrows(true, true));
    CHECK_FALSE(leavesArrows(false, false));
}

// 0x00965960, 0x00962ea0
TEST_CASE("what an edit box cannot hold goes off its end", "[markup][editbox]") {
    using wowee::ui::codepoints;
    using wowee::ui::insertIntoEditBox;
    using wowee::ui::visibleLength;
    std::string t = "abcde";
    CHECK(insertIntoEditBox(t, 0, "X", -1, 5, false) == 1);
    CHECK(t == "Xabcd");
    t = "abcde";
    CHECK(insertIntoEditBox(t, 2, "YZ", -1, 5, false) == 4);
    CHECK(t == "abYZc");
    CHECK(insertIntoEditBox(t, 5, "Q", -1, 5, false) == 5);
    CHECK(t == "abYZc");
    CHECK(insertIntoEditBox(t, 1, "1234567", -1, 5, false) == 5);
    CHECK(t == "a1234");
    t = "abc";
    CHECK(insertIntoEditBox(t, 1, "def", 4, 0, false) == 4);
    CHECK(t == "adef");
    CHECK(insertIntoEditBox(t, 4, "g", -1, 0, false) == 5);

    // An umlaut is one letter and two bytes; a link shows its words alone.
    CHECK(visibleLength("\xC3\xA4" "b") == 2);
    CHECK(codepoints("\xC3\xA4" "b") == 2);
    t = "\xC3\xA4\xC3\xB6";
    CHECK(insertIntoEditBox(t, 0, "\xC3\xBC", -1, 3, false) == 2);
    CHECK(t == "\xC3\xBC\xC3\xA4\xC3\xB6");
    CHECK(insertIntoEditBox(t, 0, "x", 5, 0, false) == 1);
    CHECK(t == "x\xC3\xBC\xC3\xA4");
    t = "|cffffffff|Hitem:1|h[AB]|h|r";
    CHECK(insertIntoEditBox(t, 0, "xy", -1, 6, false) == 2);
    CHECK(visibleLength(t) == 6);
    CHECK(insertIntoEditBox(t, 0, "z", -1, 6, false) == 1);
    CHECK(t.substr(0, 3) == "zxy");
    CHECK(t.find("item") == std::string::npos);
    t = "|cffffffffab|r";
    CHECK(insertIntoEditBox(t, 0, "", -1, 13, true) == 0);
    CHECK(codepoints(t) <= 13);
}

// 0x006bd5a0, 0x00962d50
TEST_CASE("an edit box's steps: whole characters, and the ends of escapes with them", "[markup][editbox]") {
    const std::string umlauts = "a\xC3\xA4\xE2\x82\xAC" "b";
    CHECK(caretStepRight(umlauts, 1) == 3);
    CHECK(caretStepRight(umlauts, 3) == 6);
    CHECK(caretStepLeft(umlauts, 6) == 3);
    CHECK(caretStepLeft(umlauts, 3) == 1);
    // A link is gone with one backspace, its colour too.
    const std::string link = "x|cff9d9d9d|Hitem:1|h[AB]|h|r";
    CHECK(caretStepLeft(link, link.size()) == 1);
    CHECK(caretStepRight(link, 1) == link.size());
    CHECK(caretStepRight(link + " y", 1) == link.size());
    const std::string coloured = "|cffffffffab|rc";
    CHECK(caretStepRight(coloured, 0) == 11);
    CHECK(caretStepRight(coloured, 11) == 14);
    CHECK(caretStepLeft(coloured, 15) == 14);
    CHECK(caretStepLeft(coloured, 14) == 11);
    CHECK(caretStepLeft(coloured, 11) == 0);
    // A picture with the character after it, or alone at the end.
    const std::string picture = "a|Tp:0|tb|Tq:0|t";
    CHECK(caretStepRight(picture, 1) == 9);
    CHECK(caretStepLeft(picture, 9) == 1);
    CHECK(caretStepRight(picture, 9) == picture.size());
    CHECK(caretStepLeft(picture, picture.size()) == 9);
    // What is no escape is its characters.
    CHECK(caretStepRight("|cxyz", 0) == 1);
    CHECK(caretStepRight("|cgggggggga", 0) == 1);
    CHECK(caretStepRight("|Hitem", 0) == 1);
    CHECK(caretStepRight("|Ha|h|h", 0) == 1);
    CHECK(caretStepRight("||a", 0) == 2);
    CHECK(caretStepRight("|", 0) == 1);
    CHECK(caretStepRight("a|r", 0) == 3);
    CHECK(caretStepLeft("|r", 2) == 0);
    using wowee::ui::visibleLength;
    CHECK(visibleLength("a\nb|nc\r\nd") == 7);
    CHECK(visibleLength("||") == 1);
    CHECK(visibleLength("a|Tp:0|tb") == 2);
    CHECK(visibleLength("|cxyz") == 5);
    using wowee::ui::wordDeleteBackFrom;
    const std::string words = "ab |cffffffff  |r";
    CHECK(wordDeleteBackFrom(words, words.size()) == 14);  // the end of the colour is no blank
    CHECK(wordDeleteBackFrom("ab \xC3\xA4x ", 7) == 3);
}

// 0x00483e80, 0x006bce60
TEST_CASE("a text is as wide as what it draws", "[markup]") {
    using wowee::ui::markupWidth;
    const auto ten = [](const std::string& s) { return 10.0f * static_cast<float>(s.size()); };
    CHECK(markupWidth("abc", 12.0f, ten) == 30.0f);
    CHECK(markupWidth("|cffff0000abc|r", 12.0f, ten) == 30.0f);
    CHECK(markupWidth("|cff9d9d9d|Hitem:1:0|h[abc]|h|r", 12.0f, ten) == 50.0f);
    CHECK(markupWidth("ab|nabcd|na", 12.0f, ten) == 40.0f);
    CHECK(markupWidth("|Tx:20:30|tabc", 12.0f, ten) == 60.0f);
    CHECK(markupWidth("|Tx:20|tabc", 12.0f, ten) == 50.0f);
    CHECK(markupWidth("abc|Tx:0|t", 12.0f, ten) == 42.0f);
    CHECK(markupWidth("", 12.0f, ten) == 0.0f);
    CHECK(markupWidth("a||b", 12.0f, ten) == 30.0f);
}

// 0x006c6ec5
TEST_CASE("a link's text is its markup out of the line", "[markup]") {
    const auto textOf = [](const std::string& line) {
        for (const WrapRun& r : parseMarkup(line))
            if (!r.link.empty()) return r.linkText;
        return std::string("none");
    };
    CHECK(textOf("a |cff9d9d9d|Hitem:2092:0|h[Dolch]|h|r b") == "|cff9d9d9d|Hitem:2092:0|h[Dolch]|h|r");
    CHECK(textOf("|Hquest:1:2|h[Zehn Ratten]|h!") == "|Hquest:1:2|h[Zehn Ratten]|h");
    // The colour ends elsewhere, or was over: without it.
    CHECK(textOf("|cff9d9d9d|Hitem:1|h[A]|h und|r") == "|Hitem:1|h[A]|h");
    CHECK(textOf("|cff9d9d9dgrau|r |Hitem:1|h[A]|h|r") == "|Hitem:1|h[A]|h");
    // The colour need not be just before it.
    CHECK(textOf("|cff9d9d9dgrau |Hitem:1|h[A]|h|r") == "|cff9d9d9dgrau |Hitem:1|h[A]|h|r");
    CHECK(textOf("|Hitem:1|h[A]").empty());
    for (const WrapRun& r : parseMarkup("|Hitem:1|h[A]|h tail"))
        if (r.link.empty()) CHECK(r.linkText.empty());
}
