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
struct Tried {
    uint32_t displayInfoId;
    uint8_t inventoryType;
    bool offHand = false;
};
game::EquipmentItem item(uint32_t display, uint8_t type, uint32_t enchant = 0) {
    return {.displayModel = display, .inventoryType = type, .enchantment = enchant};
}
}  // namespace

TEST_CASE("what is tried on takes the place of what is worn there", "[unit_look]") {
    const std::vector<game::EquipmentItem> worn{item(100, 1), item(200, 5, 7), item(300, 17)};
    CHECK(ui::wornWithTriedOn(worn, false, std::vector<Tried>{}) == worn);
    CHECK(ui::wornWithTriedOn(worn, true, std::vector<Tried>{}).empty());
    const auto two = ui::wornWithTriedOn(worn, false, std::vector<Tried>{{900, 5}, {901, 7}});
    REQUIRE(two.size() == 4);
    CHECK(two[1] == item(900, 5));
    CHECK(two[3] == item(901, 7));
    const auto bare = ui::wornWithTriedOn(worn, true, std::vector<Tried>{{900, 5}});
    REQUIRE(bare.size() == 1);
    CHECK(bare[0] == item(900, 5));
}

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

// 0x005980d0, 0x00597c50
TEST_CASE("what is tried on is put where it belongs", "[unit_look]") {
    using Items = std::vector<game::EquipmentItem>;
    using Try = std::vector<Tried>;
    // A vest for a robe.
    CHECK(ui::wornWithTriedOn(Items{item(1, 20), item(2, 7)}, false, Try{{900, 5}}) == Items{item(900, 5), item(2, 7)});
    // A two-hand weapon for the one held, a shield puts a two-hand one away, a weapon for
    // two hands the shield.
    CHECK(ui::wornWithTriedOn(Items{item(1, 17)}, false, Try{{900, 17}}) == Items{item(900, 17)});
    CHECK(ui::wornWithTriedOn(Items{item(1, 17)}, false, Try{{900, 14}}) == Items{item(900, 14)});
    CHECK(ui::wornWithTriedOn(Items{item(1, 13), item(2, 14)}, false, Try{{900, 17}}) == Items{item(900, 17)});
    CHECK(ui::wornWithTriedOn(Items{item(1, 13), item(2, 14)}, false, Try{{900, 21}}) == Items{item(900, 21), item(2, 14)});
    // A second weapon is the off hand's, and stays beside a one-hand one of who holds two.
    CHECK(ui::wornWithTriedOn(Items{item(1, 13), item(2, 13)}, false, Try{{900, 13}}, true) ==
          Items{item(900, 13), item(2, 22)});
    CHECK(ui::wornWithTriedOn(Items{item(1, 13), item(2, 13)}, false, Try{{900, 13}}) == Items{item(900, 13)});
    CHECK(ui::wornWithTriedOn(Items{item(1, 13)}, false, Try{{900, 13, true}}, true) == Items{item(1, 13), item(900, 22)});
    // A bow on the back is not in a hand beside a sword.
    CHECK(ui::wornWithTriedOn(Items{item(1, 13), item(2, 15)}, false, Try{{900, 1}}) == Items{item(1, 13), item(900, 1)});
    // A ring is nowhere.
    CHECK(ui::wornWithTriedOn(Items{item(1, 1)}, false, Try{{900, 11}}) == Items{item(1, 1)});
    CHECK(ui::dressPlace(20) == ui::dressPlace(5));
    CHECK(ui::dressPlace(15) == ui::kDressOffHand);
    CHECK(ui::dressPlace(0) == ui::kDressNowhere);
}
