// What a unit looks like in a portrait or a model frame (ui/unit_look.hpp).
#include <catch_amalgamated.hpp>

#include "ui/unit_look.hpp"

using namespace wowee;

TEST_CASE("a player is drawn as its character while its display is its own", "[unit_look]") {
    CHECK(ui::drawnAsCharacter(true, 15476, 15476));
    // A ghost wolf, a sheep, a disguise.
    CHECK_FALSE(ui::drawnAsCharacter(true, 4613, 15476));
    // Not known yet.
    CHECK(ui::drawnAsCharacter(true, 0, 0));
    CHECK(ui::drawnAsCharacter(true, 15476, 0));
    CHECK(ui::drawnAsCharacter(true, 0, 15476));
    // Anything else is its display.
    CHECK_FALSE(ui::drawnAsCharacter(false, 26772, 26772));
    CHECK_FALSE(ui::drawnAsCharacter(false, 0, 0));
}

namespace {
game::EquipmentItem item(uint32_t display, uint8_t type, uint32_t enchant = 0) {
    return {.displayModel = display, .inventoryType = type, .enchantment = enchant};
}
}  // namespace

TEST_CASE("a look is another where anything of it is", "[unit_look]") {
    ui::UnitLook wolf;
    CHECK_FALSE(wolf.any());
    wolf.model = "Creature\\Wolf\\Wolf.m2";
    wolf.skins = {{11, "Creature\\Wolf\\WolfSkinBrown.blp"}};
    CHECK(wolf.any());
    // The same model in other colours.
    ui::UnitLook black = wolf;
    CHECK(black == wolf);
    black.skins[0].second = "Creature\\Wolf\\WolfSkinBlack.blp";
    CHECK_FALSE(black == wolf);
    ui::UnitLook elf;
    elf.character = true;
    elf.race = 10;
    CHECK(elf.any());
    ui::UnitLook dressed = elf;
    dressed.worn.push_back(item(1, 5));
    CHECK_FALSE(dressed == elf);
}
