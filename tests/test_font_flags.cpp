// A font's flags (ui/font_flags.hpp).
#include <catch_amalgamated.hpp>

#include "ui/font_flags.hpp"
#include "ui/font_follow.hpp"

using namespace wowee::ui::font_flags;

// 0x008151a0, 0x008151e0
TEST_CASE("a font's flags and their names", "[font_flags]") {
    CHECK(parse("") == 0);
    CHECK(parse("outline") == kOutline);
    CHECK(parse("THICKOUTLINE") == (kOutline | kThick));
    CHECK(parse("OUTLINE, MONOCHROME") == (kOutline | kMonochrome));
    CHECK(parse("MONOCHROMETHICKOUTLINE") == (kOutline | kThick | kMonochrome));
    CHECK(parse("NORMAL") == 0);
    CHECK(names(0).empty());
    CHECK(names(kOutline) == "OUTLINE");
    CHECK(names(kOutline | kThick | kMonochrome) == "OUTLINE, THICKOUTLINE, MONOCHROME");
    CHECK(names(parse(names(kOutline | kThick))) == "OUTLINE, THICKOUTLINE");
    CHECK(ofXml("NORMAL", false) == kOutline);
    CHECK(ofXml("thick", true) == (kOutline | kThick | kMonochrome));
    CHECK(ofXml("NONE", false) == 0);
    CHECK(ofXml("", true) == kMonochrome);
}

// 0x00483030, 0x004963f0
TEST_CASE("what a text takes of a font object", "[font_flags]") {
    namespace ff = wowee::ui::font_follow;
    CHECK(ff::taken(ff::kAll, 0) == ff::kAll);
    CHECK(ff::taken(ff::kAll, ff::kColor) == (ff::kFont | ff::kStyle | ff::kShadow | ff::kSpacing));
    CHECK(ff::taken(ff::kColor, ff::kColor | ff::kFont) == 0);
    CHECK(ff::taken(ff::kShadow, ff::kColor) == ff::kShadow);
    CHECK(ff::taken(0xff, 0) == ff::kAll);
}
