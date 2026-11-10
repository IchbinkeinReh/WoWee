// The minimap's replaceable textures (Minimap:Set*Texture, 0x00583860..).
#include <catch_amalgamated.hpp>

#include "rendering/minimap_art.hpp"

using namespace wowee::rendering::minimap_art;

TEST_CASE("The minimap draws its defaults until the interface names others", "[minimap_art]") {
    REQUIRE(pathOf(Blips) == "Interface\\Minimap\\ObjectIcons.blp");
    REQUIRE(pathOf(POIArrow) == "Interface\\Minimap\\Rotating-MinimapGuideArrow.blp");
    const uint32_t before = state().generation;
    set(Blips, "Interface\\AddOns\\X\\Blips");
    REQUIRE(pathOf(Blips) == "Interface\\AddOns\\X\\Blips.blp");
    REQUIRE(state().generation == before + 1);
    set(Blips, "");                      // an empty name changes nothing
    set(Blips, "Interface\\AddOns\\X\\Blips.blp");   // nor does the same file
    REQUIRE(state().generation == before + 1);
    REQUIRE(normalized("a.b\\c") == "a.b\\c.blp");
}
