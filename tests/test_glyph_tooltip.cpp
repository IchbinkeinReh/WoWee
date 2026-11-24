// A glyph socket's tooltip (0x00625d00 -> 0x00622ba0).
#include <catch_amalgamated.hpp>

#include "game/glyph_tooltip.hpp"

namespace gt = wowee::game::glyph_tooltip;

namespace {
std::string key(const std::string& k) { return k; }
std::vector<std::string> texts(const std::vector<gt::Line>& lines) {
    std::vector<std::string> out;
    for (const auto& l : lines) out.push_back(l.text);
    return out;
}
using Texts = std::vector<std::string>;
}  // namespace

TEST_CASE("a socket with nothing in it says so", "[glyph]") {
    gt::Socket s;
    s.slotKnown = true;
    s.slotType = 1;
    s.slotTooltip = 4;
    s.enabled = true;
    auto l = gt::lines(s, key);
    CHECK(texts(l) == Texts{"GLYPH_INACTIVE", "MINOR_GLYPH", "GLYPH_EMPTY_DESC"});
    CHECK(l[0].color == gt::Color::Grey);
    CHECK(l[1].color == gt::Color::Blue);
    CHECK(l[2].color == gt::Color::Gold);
    CHECK(l[2].wraps);
    // One not open yet says when it is.
    s.enabled = false;
    s.slotType = 0;
    l = gt::lines(s, key);
    CHECK(texts(l) == Texts{"GLYPH_LOCKED", "MAJOR_GLYPH", "GLYPH_SLOT_TOOLTIP4"});
    CHECK(l[0].color == gt::Color::Red);
    s.slotKnown = false;
    CHECK(texts(gt::lines(s, key)) == Texts{"GLYPH_LOCKED", "MAJOR_GLYPH"});
}

TEST_CASE("a glyph says what it does, and in its socket how to take it out", "[glyph]") {
    gt::Socket s;
    s.slotKnown = s.enabled = s.filled = true;
    s.spellName = "Glyphe 'Verbrennen'";
    s.description = "Erhöht den Schaden.";
    auto l = gt::lines(s, key);
    CHECK(texts(l) == Texts{"Glyphe 'Verbrennen'", "MAJOR_GLYPH", "Erhöht den Schaden.", "GLYPH_SLOT_REMOVE_TOOLTIP"});
    CHECK(l[0].color == gt::Color::White);
    CHECK(l[3].color == gt::Color::Grey);
    CHECK_FALSE(l[3].wraps);
    s.link = true;
    CHECK(texts(gt::lines(s, key)) == Texts{"Glyphe 'Verbrennen'", "MAJOR_GLYPH", "Erhöht den Schaden."});
    s.description.clear();
    CHECK(texts(gt::lines(s, key)) == Texts{"Glyphe 'Verbrennen'", "MAJOR_GLYPH"});
    // A spell that is not there has no name, and is as none.
    s.spellName.reset();
    CHECK(texts(gt::lines(s, key)) == Texts{"MAJOR_GLYPH", "GLYPH_EMPTY_DESC"});
}
