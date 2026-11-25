// A font's flags (ui/font_flags.hpp).
#include <catch_amalgamated.hpp>

#include "ui/font_flags.hpp"

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
