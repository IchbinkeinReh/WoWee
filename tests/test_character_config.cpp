// A character's CVars (addons/character_config.hpp).
#include <catch_amalgamated.hpp>

#include "addons/character_config.hpp"

#include <algorithm>

using namespace wowee::addons;

namespace {
std::string valueOf(const std::vector<std::pair<std::string, std::string>>& values, const std::string& key) {
    const auto it = std::find_if(values.begin(), values.end(), [&](const auto& kv) { return kv.first == key; });
    return it == values.end() ? std::string("-") : it->second;
}
}  // namespace

TEST_CASE("config-cache.wtf's lines", "[character_config]") {
    const auto values = configCacheValues("SET minimapZoom \"3\"\r\nBLAH\nSET questLogCollapseFilter \"-1\"\nSET x\n"
                                          "SET lfdSelectedDungeons \"\x02""A\"");
    CHECK(values.size() == 3);
    CHECK(values.at("minimapzoom") == "3");
    CHECK(values.at("questlogcollapsefilter") == "-1");
    CHECK(values.at("lfdselecteddungeons") == "\x02""A");
}

// 0x00767350, 0x00518bf0
TEST_CASE("a character has its own CVars, the others on their default", "[character_config]") {
    const auto values = characterCVars("SET minimapZoom \"2\"\nSET autoLootDefault \"1\"\n");
    CHECK(valueOf(values, "minimapzoom") == "2");
    CHECK(valueOf(values, "lfgselectedroles") == "0");
    CHECK(valueOf(values, "lfdcollapsedheaders").empty());
    CHECK(valueOf(values, "showtokenframe") != "-");
    // The account's are not the character's.
    CHECK(valueOf(values, "autolootdefault") == "-");
    CHECK(valueOf(values, "addfriendinfoshown") == "-");
}

TEST_CASE("where they are kept", "[character_config]") {
    CHECK(characterConfigCacheFile("c", "Locki") == "c/characters/Locki.config-cache.wtf");
    CHECK(characterConfigCacheFile("c", "").empty());
}
