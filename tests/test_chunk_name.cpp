// The name of a file's chunk (addons/chunk_name.hpp).
#include <catch_amalgamated.hpp>

#include "addons/chunk_name.hpp"

using wowee::addons::chunkNameOf;

// 0x00818f60
TEST_CASE("a file's chunk is named by the path it is asked for by", "[chunk_name]") {
    CHECK(chunkNameOf("H:\\wowee-play\\p-1\\addons\\TstA\\sub\\a.lua", "TstA") ==
          "@Interface\\AddOns\\TstA\\sub\\a.lua");
    CHECK(chunkNameOf("H:/cache/interface/AddOns\\blizzard_raidui/Blizzard_RaidUI.lua", "Blizzard_RaidUI") ==
          "@Interface\\AddOns\\Blizzard_RaidUI\\Blizzard_RaidUI.lua");
    CHECK(chunkNameOf("H:/cache/interface_mpq/interface/FrameXML\\UIParent.lua") ==
          "@Interface\\FrameXML\\UIParent.lua");
    CHECK(chunkNameOf("H:/x/interface/AddOns/blizzard_inspectui/../../FrameXML/PVPFrameTemplates.lua") ==
          "@Interface\\FrameXML\\PVPFrameTemplates.lua");
    CHECK(chunkNameOf("H:/x/addons/Other/addons/b.lua", "TstA") == "@Interface\\AddOns\\Other\\addons\\b.lua");
    CHECK(chunkNameOf("H:/x/b.lua") == "@H:/x/b.lua");
}
