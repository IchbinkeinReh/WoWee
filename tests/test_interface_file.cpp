// The file a texture's name stands for (ui/interface_file.hpp).
#include <catch_amalgamated.hpp>

#include "ui/interface_file.hpp"

using wowee::ui::textureFileOf;

TEST_CASE("a texture is the BLP of its name", "[interface_file]") {
    CHECK(textureFileOf("Interface\\Icons\\INV_Sword_04") == "Interface\\Icons\\INV_Sword_04.blp");
    CHECK(textureFileOf("Interface\\Icons\\INV_Sword_04.blp") == "Interface\\Icons\\INV_Sword_04.blp");
    CHECK(textureFileOf("Interface\\Icons\\INV_Sword_04.BLP") == "Interface\\Icons\\INV_Sword_04.BLP");
    CHECK(textureFileOf("Interface\\QuestIcon.tga") == "Interface\\QuestIcon.blp");
    CHECK(textureFileOf("Interface\\QuestIcon.TGA") == "Interface\\QuestIcon.blp");
    CHECK(textureFileOf("Spell.v2") == "Spell.v2.blp");
    CHECK(textureFileOf("a") == "a.blp");
    CHECK(textureFileOf(".blp") == ".blp.blp");
}
