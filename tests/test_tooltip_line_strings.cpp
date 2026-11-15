// A tooltip's lines held by its TextLeft<n> / TextRight<n> font strings.
//
// In the client the lines ARE those font strings (AddLine, 0x0061fec0), and
// addons read a tooltip through them: `_G[name.."TextLeft"..i]:GetText()` for
// i up to NumLines(). This client draws a tooltip from Widget::tooltipLines,
// and the font strings held nothing - every tooltip scanner read nil. These
// pin the mirror both ways, on the tree alone; the Lua side only decides when
// it runs.
#include <catch_amalgamated.hpp>

#include "ui/tooltip_line_strings.hpp"
#include "ui/widget_tree.hpp"

#include <string>
#include <vector>

using namespace wowee::ui;

namespace {

Widget::TooltipLine line(const std::string& left, const std::string& right = {}) {
    Widget::TooltipLine l;
    l.left = left;
    l.right = right;
    for (int i = 0; i < 4; ++i) l.lc[i] = l.rc[i] = 1.0f;
    return l;
}

/// GameTooltip as the template leaves it: two pairs of line font strings.
struct Fixture {
    WidgetTree tree;
    uint32_t tip = 0;
    std::vector<std::string> made;

    Fixture() {
        tip = tree.create(WidgetKind::Frame, 0, "GameTooltip");
        Widget* w = tree.get(tip);
        w->objectType = "GameTooltip";
        w->isTooltip = true;
        for (int n = 1; n <= 2; ++n) {
            for (const char* side : {"TextLeft", "TextRight"}) {
                const uint32_t id = tree.create(WidgetKind::FontString, tip,
                                                std::string("GameTooltip") + side + std::to_string(n));
                tree.get(id)->shown = false;
            }
        }
    }

    /// What the Lua side does: a new font string under the asked-for name.
    MakeTooltipLineString maker() {
        return [this](std::size_t n, bool right) {
            const std::string name = tooltipLineStringName("GameTooltip", n, right);
            made.push_back(name);
            const uint32_t id = tree.create(WidgetKind::FontString, tip, name);
            tree.get(id)->shown = false;
            return id;
        };
    }

    const Widget& fs(const char* name) {
        const Widget* w = tree.findByName(name);
        REQUIRE(w != nullptr);
        return *w;
    }
    Widget& tooltip() { return *tree.get(tip); }
};

}  // namespace

TEST_CASE("the line font strings hold the lines, and the spare pair is made", "[tooltip]") {
    Fixture f;
    f.tooltip().tooltipLines.push_back(line("Schattenfrostdolch"));
    auto bound = line("Seelengebunden");
    bound.lc[0] = 0.5f;
    f.tooltip().tooltipLines.push_back(bound);
    f.tooltip().tooltipLines.push_back(line("Einhand", "Dolch"));

    REQUIRE(mirrorTooltipLines(f.tree, f.tip, f.maker()) == 3);

    CHECK(f.fs("GameTooltipTextLeft1").text == "Schattenfrostdolch");
    CHECK(f.fs("GameTooltipTextLeft1").shown);
    CHECK(f.fs("GameTooltipTextLeft2").text == "Seelengebunden");
    CHECK(f.fs("GameTooltipTextLeft2").color[0] == Catch::Approx(0.5f));
    CHECK(f.fs("GameTooltipTextLeft3").text == "Einhand");
    CHECK(f.fs("GameTooltipTextRight3").text == "Dolch");
    CHECK(f.fs("GameTooltipTextRight3").shown);
    // A side without text is not shown (0x0061fec0 shows only non-empty ones).
    CHECK(f.fs("GameTooltipTextRight1").text.empty());
    CHECK_FALSE(f.fs("GameTooltipTextRight1").shown);
    // Lines 3 and the spare 4 were made, in pairs; 1 and 2 came from the template.
    CHECK(f.made == std::vector<std::string>{"GameTooltipTextLeft3", "GameTooltipTextRight3",
                                             "GameTooltipTextLeft4", "GameTooltipTextRight4"});
    CHECK_FALSE(f.fs("GameTooltipTextLeft4").shown);
    // Marked as the tooltip's, so the renderer leaves the drawing to it.
    CHECK(f.fs("GameTooltipTextLeft3").tooltipLineOwner == f.tip);
    CHECK(f.fs("GameTooltipTextRight3").tooltipLineRight);
}

TEST_CASE("fewer lines empty and hide the rest, as ClearLines does", "[tooltip]") {
    Fixture f;
    for (const char* t : {"a", "b", "c"}) f.tooltip().tooltipLines.push_back(line(t, "r"));
    mirrorTooltipLines(f.tree, f.tip, f.maker());
    f.tooltip().tooltipLines.clear();
    f.tooltip().tooltipLines.push_back(line("only"));
    f.made.clear();
    mirrorTooltipLines(f.tree, f.tip, f.maker());

    CHECK(f.made.empty());  // nothing new needed
    CHECK(f.fs("GameTooltipTextLeft1").text == "only");
    CHECK(f.fs("GameTooltipTextRight1").text.empty());
    CHECK_FALSE(f.fs("GameTooltipTextRight1").shown);
    for (const char* n : {"GameTooltipTextLeft2", "GameTooltipTextLeft3", "GameTooltipTextRight3"}) {
        INFO(n);
        CHECK(f.fs(n).text.empty());
        CHECK_FALSE(f.fs(n).shown);
    }
}

TEST_CASE("a write to a line font string reaches the line it stands for", "[tooltip]") {
    Fixture f;
    f.tooltip().tooltipLines.push_back(line("Ragnaros"));
    f.tooltip().tooltipLines.push_back(line("Stufe ??"));
    mirrorTooltipLines(f.tree, f.tip, f.maker());

    // OnTooltipSetUnit: TextLeft1:SetTextColor(GameTooltip_UnitColor(unit)).
    Widget& name = *f.tree.get(f.fs("GameTooltipTextLeft1").id);
    name.color[0] = 1.0f;
    name.color[1] = 0.0f;
    name.color[2] = 0.0f;
    REQUIRE(tooltipLineStringChanged(f.tree, name));
    CHECK(f.tooltip().tooltipLines[0].lc[1] == Catch::Approx(0.0f));

    // An addon rewriting a line.
    Widget& second = *f.tree.get(f.fs("GameTooltipTextLeft2").id);
    second.text = "Stufe 63 Elite";
    REQUIRE(tooltipLineStringChanged(f.tree, second));
    CHECK(f.tooltip().tooltipLines[1].left == "Stufe 63 Elite");

    // Hidden by hand: the row stays, the side is not drawn, and the next
    // mirror keeps it hidden rather than showing it again.
    second.shown = false;
    REQUIRE(tooltipLineStringChanged(f.tree, second));
    CHECK_FALSE(f.tooltip().tooltipLines[1].leftVisible());
    mirrorTooltipLines(f.tree, f.tip, f.maker());
    CHECK_FALSE(f.fs("GameTooltipTextLeft2").shown);
    CHECK(f.fs("GameTooltipTextLeft2").text == "Stufe 63 Elite");

    // Text given to a hidden empty side does not show it (it is shown by Show).
    Widget& right = *f.tree.get(f.fs("GameTooltipTextRight2").id);
    right.text = "x";
    REQUIRE(tooltipLineStringChanged(f.tree, right));
    CHECK(f.tooltip().tooltipLines[1].right == "x");
    CHECK_FALSE(f.tooltip().tooltipLines[1].rightVisible());

    // Past NumLines(): the font string changes, no line does.
    Widget& spare = *f.tree.get(f.fs("GameTooltipTextLeft3").id);
    spare.text = "nobody";
    CHECK_FALSE(tooltipLineStringChanged(f.tree, spare));
    CHECK(f.tooltip().tooltipLines.size() == 2);
}

TEST_CASE("a rebuilt line follows AddLine's rule again", "[tooltip]") {
    Fixture f;
    f.tooltip().tooltipLines.push_back(line("a"));
    mirrorTooltipLines(f.tree, f.tip, f.maker());
    Widget& first = *f.tree.get(f.fs("GameTooltipTextLeft1").id);
    first.shown = false;
    tooltipLineStringChanged(f.tree, first);
    // The tooltip filled again: a new line, shown because it has text.
    f.tooltip().tooltipLines.clear();
    f.tooltip().tooltipLines.push_back(line("a"));
    mirrorTooltipLines(f.tree, f.tip, f.maker());
    CHECK(f.fs("GameTooltipTextLeft1").shown);
}

TEST_CASE("only a named GameTooltip has line font strings", "[tooltip]") {
    WidgetTree tree;
    const uint32_t frame = tree.create(WidgetKind::Frame, 0, "SomeFrame");
    tree.get(frame)->isTooltip = true;  // a stray setter call
    tree.get(frame)->tooltipLines.push_back(line("x"));
    int asked = 0;
    const MakeTooltipLineString make = [&](std::size_t, bool) { ++asked; return 0u; };
    CHECK(mirrorTooltipLines(tree, frame, make) == 0);
    CHECK(asked == 0);
    CHECK(tree.findByName("SomeFrameTextLeft1") == nullptr);

    const uint32_t unnamed = tree.create(WidgetKind::Frame, 0, "");
    tree.get(unnamed)->objectType = "GameTooltip";
    tree.get(unnamed)->tooltipLines.push_back(line("x"));
    CHECK(mirrorTooltipLines(tree, unnamed, make) == 0);
    CHECK(asked == 0);
}

TEST_CASE("lines made of an addon's own font strings leave the named ones hidden", "[tooltip]") {
    Fixture f;
    auto adopted = line("mine", "too");
    adopted.leftFontString = 999;
    f.tooltip().tooltipLines.push_back(adopted);
    mirrorTooltipLines(f.tree, f.tip, f.maker());
    CHECK(f.fs("GameTooltipTextLeft1").text.empty());
    CHECK_FALSE(f.fs("GameTooltipTextLeft1").shown);
}
