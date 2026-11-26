// T339, the user's decisions of 2026-11-26: three departures from the 12340
// interface go - the browse list keeps the realm's order (no client sort
// before it is drawn), the talent frame previews as FrameXML does (no lit
// picks, nothing reset on hide), and the option panels keep every control
// and page FrameXML builds (nothing taken off them).
#include <catch_amalgamated.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace {
std::string sourceText(const char* path) {
    std::ifstream in(std::string(WOWEE_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("the interface's departures the user took back are gone", "[original_interface]") {
    const std::string manager = sourceText("src/addons/addon_manager.cpp");
    const std::string snippets = sourceText("include/addons/addon_lua_snippets.hpp");
    REQUIRE_FALSE(manager.empty());
    REQUIRE_FALSE(snippets.empty());
    // (a) the auction browse list in the realm's order.
    CHECK(manager.find("AuctionFrameBrowse_Update = function") == std::string::npos);
    // (b) the talent preview as FrameXML has it.
    CHECK(manager.find("kTalentPreviewLua") == std::string::npos);
    CHECK(snippets.find("kTalentPreviewLua") == std::string::npos);
    // (c) every option control and page there.
    CHECK(manager.find("kRemovedControlsLua") == std::string::npos);
    CHECK(snippets.find("kRemovedControlsLua") == std::string::npos);
    CHECK(snippets.find("\"AudioOptionsSoundPanelEmoteSounds\"") == std::string::npos);
}
