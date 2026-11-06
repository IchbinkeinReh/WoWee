// The durability figure's alerts as 0x005e8fe0 works them out.
#include <catch_amalgamated.hpp>

#include "game/inventory_alerts.hpp"

namespace ia = wowee::game::inventory_alerts;

TEST_CASE("an item is low with fewer than six points and broken with none", "[alerts]") {
    ia::Item it{.present = true, .fieldFlags = 0, .durability = 50, .maxDurability = 100};
    CHECK(ia::itemAlert(it).status == 0);
    it.durability = 6;
    CHECK(ia::itemAlert(it).status == 0);
    it.durability = 5;
    CHECK(ia::itemAlert(it).status == 1);
    CHECK(ia::itemAlert(it).tutorial == 0x23);
    it.durability = 0;
    CHECK(ia::itemAlert(it).status == 2);
    CHECK(ia::itemAlert(it).tutorial == 0x24);
}

TEST_CASE("the flags and an item without durability", "[alerts]") {
    ia::Item it{.present = true, .fieldFlags = ia::kFieldFlagBroken, .durability = 50, .maxDurability = 100};
    CHECK(ia::itemAlert(it).status == 2);
    CHECK(ia::itemAlert(it).tutorial == -1);
    it.fieldFlags = ia::kFieldFlagNoDurability;
    it.durability = 0;
    CHECK(ia::itemAlert(it).status == 0);
    it = {.present = true, .fieldFlags = 0, .durability = 0, .maxDurability = 0};
    CHECK(ia::itemAlert(it).status == 0);
    CHECK(ia::itemAlert({}).status == 0);
}

TEST_CASE("the ammunition is low under 21 rounds", "[alerts]") {
    CHECK(ia::ammoAlert(0, 0) == 0);
    CHECK(ia::ammoAlert(2512, 20) == 1);
    CHECK(ia::ammoAlert(2512, 21) == 0);
}
