// The files a texture's name stands for (ui/interface_file.hpp).
#include <catch_amalgamated.hpp>

#include "ui/interface_file.hpp"

using wowee::ui::textureFileOf;
using wowee::ui::textureTgaOf;

// 0x004b9760, 0x004b51c0
TEST_CASE("a texture is the BLP of its name", "[interface_file]") {
    CHECK(textureFileOf("Interface\\Icons\\INV_Sword_04") == "Interface\\Icons\\INV_Sword_04.blp");
    CHECK(textureFileOf("Interface\\Icons\\INV_Sword_04.BLP") == "Interface\\Icons\\INV_Sword_04.blp");
    CHECK(textureFileOf("Interface\\QuestIcon.tga") == "Interface\\QuestIcon.blp");
    CHECK(textureFileOf("Interface/QuestIcon.TGA") == "Interface/QuestIcon.blp");
    CHECK(textureFileOf("Spell.v2") == "Spell.blp");
    CHECK(textureFileOf("a") == "a.blp");
    CHECK(textureFileOf("AddOns.x\\a") == "AddOns.x\\a.blp");
    CHECK(textureFileOf("AddOns.x/a") == "AddOns.x/a.blp");
    CHECK(textureTgaOf("Interface\\AddOns\\TstM\\kreis.blp") == "Interface\\AddOns\\TstM\\kreis.tga");
    CHECK(textureTgaOf("kreis") == "kreis.tga");
}
