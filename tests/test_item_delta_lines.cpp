// The comparison tooltip's changes (game/item_delta_lines.hpp).
#include <catch_amalgamated.hpp>

#include "game/item_delta_lines.hpp"

using namespace wowee::game::item_stats;

namespace {
std::vector<std::string> linesOf(const Summary& s) {
    return deltaLines(s, [](const std::string& k) { return "<" + k + ">"; });
}
}  // namespace

// 0x0062d930
TEST_CASE("what changes with an item in the place of the worn one", "[item_delta]") {
    Summary s;
    CHECK(linesOf(s).empty());
    s.dps = -12.34f;
    s.v[0] = 55;
    s.v[kFirstMod + 3] = -7;
    s.v[0x31] = 9;
    s.v[0x32] = 9;
    s.v[kFirstSocket + 1] = 1;
    const auto lines = linesOf(s);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "|cffff2020-12.3|r <ITEM_MOD_DAMAGE_PER_SECOND_SHORT>");
    CHECK(lines[1] == "|cff00ff00+55|r <" + key(0) + ">");
    CHECK(lines[2] == "|cffff2020-7|r <" + key(kFirstMod + 3) + ">");
    CHECK(lines[3] == "|cff00ff00+1|r |TInterface\\ItemSocketingFrame\\UI-EmptySocket-Red.blp:12|t  <" +
                          key(kFirstSocket + 1) + ">");
}
