// SetIndentedWordWrap's wrap (0x006bd390, 0x006bdb10): lines after the first
// are narrower by the indent.
#include <catch_amalgamated.hpp>

#include "ui/text_wrap.hpp"

using namespace wowee::ui;

TEST_CASE("An indented wrap gives later lines less room", "[text_wrap]") {
    // Every character is one unit wide.
    auto measure = [](const std::string& s) { return static_cast<float>(s.size()); };
    std::vector<WrapRun> runs(1);
    runs[0].text = "aaaa bbbb cccc dddd";
    const auto plain = wrapText(runs, 10.0f, false, measure);
    const auto indented = wrapText(runs, 10.0f, false, measure, 6.0f);
    REQUIRE(plain.size() == 2);       // "aaaa bbbb " / "cccc dddd"
    REQUIRE(indented.size() == 3);    // "aaaa bbbb " / "cccc " / "dddd"
    REQUIRE(indented[1][0].text == "cccc ");
}
