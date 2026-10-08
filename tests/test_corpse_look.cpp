// What a CORPSE object's fields say it looks like (0x00705670, 0x00705b20).
#include <catch_amalgamated.hpp>

#include "game/corpse_look.hpp"

using namespace wowee::game;

TEST_CASE("a corpse's bytes are its race, sex and look", "[corpse]") {
    std::array<uint32_t, 19> items{};
    // BYTES_1: race 4, sex 1, skin 3; BYTES_2: face 2, hair 5, colour 6, facial 7.
    const CorpseLook look = corpseLook(55, items, 0x03010400u, 0x07060502u, 0);
    CHECK(look.displayId == 55);
    CHECK(look.race == 4);
    CHECK(look.gender == 1);
    CHECK(look.skin == 3);
    CHECK(look.face == 2);
    CHECK(look.hairStyle == 5);
    CHECK(look.hairColor == 6);
    CHECK(look.facialHair == 7);
    CHECK(look.appearanceBytes() == 0x06050203u);
    CHECK_FALSE(look.bones());
}

TEST_CASE("a corpse wears its armour but no weapons (0x00705b20)", "[corpse]") {
    std::array<uint32_t, 19> items{};
    for (uint32_t slot = 0; slot < 19; ++slot) items[slot] = (100 + slot) | ((slot + 1) << 24);
    const CorpseLook look = corpseLook(55, items, 0, 0, 0);
    CHECK(look.displayIds[0] == 100);
    CHECK(look.inventoryTypes[0] == 1);
    CHECK(look.displayIds[4] == 104);
    CHECK(look.displayIds[14] == 114);
    CHECK(look.displayIds[18] == 118);
    CHECK(look.displayIds[15] == 0);
    CHECK(look.displayIds[16] == 0);
    CHECK(look.displayIds[17] == 0);
    // Flags 8 and 0x10 take the helm and the cloak off.
    const CorpseLook hidden = corpseLook(55, items, 0, 0, kCorpseFlagHideHelm | kCorpseFlagHideCloak);
    CHECK(hidden.displayIds[0] == 0);
    CHECK(hidden.displayIds[14] == 0);
    CHECK(hidden.displayIds[4] == 104);
}

TEST_CASE("bones are the race's death skeleton (0x00705670)", "[corpse]") {
    const CorpseLook bones = corpseLook(0, {}, 0, 0, kCorpseFlagBones);
    CHECK(bones.bones());
    CHECK(corpseBonesModelPath(5, 0) ==
          "World\\Generic\\PassiveDoodads\\DeathSkeletons\\ScourgeMaleDeathSkeleton.m2");
    CHECK(corpseBonesModelPath(10, 1) ==
          "World\\Generic\\PassiveDoodads\\DeathSkeletons\\BloodElfFemaleDeathSkeleton.m2");
}

TEST_CASE("a corpse lies drowned under more than two thirds of a yard of liquid (0x00705b20)", "[corpse]") {
    CHECK(corpsePoseAnimation(std::nullopt, 10.0f) == kCorpseAnimDead);
    CHECK(corpsePoseAnimation(10.5f, 10.0f) == kCorpseAnimDead);
    CHECK(corpsePoseAnimation(10.6f, 10.0f) == kCorpseAnimDead);
    CHECK(corpsePoseAnimation(10.7f, 10.0f) == kCorpseAnimDrowned);
    // Liquid below it is no matter.
    CHECK(corpsePoseAnimation(5.0f, 10.0f) == kCorpseAnimDead);
}

TEST_CASE("a corpse's dynamic flag 1 is lootable (0x00705900)", "[corpse]") {
    CorpseLook look;
    CHECK_FALSE(look.lootable());
    look.dynamicFlags = kCorpseDynamicFlagLootable;
    CHECK(look.lootable());
}
