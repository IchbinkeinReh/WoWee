#include <catch_amalgamated.hpp>

#include "game/unit_pointer.hpp"

using namespace wowee::game::unit_pointer;

TEST_CASE("an enemy wears the sword, out of reach the sword that cannot", "[pointer]") {
    CursorAsk a{.distanceSq = 16.0f, .attackable = true, .meleeReach = 5.0f};
    CHECK(cursor(a) == 4);
    a.distanceSq = 25.0f;
    CHECK(cursor(a) == 4);
    a.distanceSq = 26.0f;
    CHECK(cursor(a) == 30);
    for (bool CursorAsk::*flag : {&CursorAsk::pacified}) {
        CursorAsk b = a;
        b.*flag = true;
        CHECK(cursor(b) == 0);
    }
    a.hasControl = false;
    CHECK(cursor(a) == 0);
    CHECK(cursor({}) == 0);
}

TEST_CASE("a corpse wears the hand, or the bag where looting is automatic", "[pointer]") {
    CursorAsk a{.alive = false, .lootable = true, .mayLoot = true};
    CHECK(cursor(a) == 8);
    a.autoLoots = true;
    CHECK(cursor(a) == 16);
    a.mayLoot = false;
    CHECK(cursor(a) == 42);
    a.autoLoots = false;
    CHECK(cursor(a) == 34);
    // Lootable before skinnable, skinnable before an attack.
    a.skinnable = true;
    CHECK(cursor(a) == 34);
    a.lootable = false;
    a.attackable = true;
    a.skinReach = 5.0f;
    a.distanceSq = 25.0f;
    CHECK(cursor(a) == 12);
    a.skinKind = 1;
    CHECK(cursor(a) == 13);
    a.skinKind = 2;
    CHECK(cursor(a) == 11);
    a.skinKind = 3;
    CHECK(cursor(a) == 11);
    a.distanceSq = 26.0f;
    CHECK(cursor(a) == 37);
    a.isPlayer = true;
    CHECK(cursor(a) == 46);
    a.playerIsHorde = true;
    CHECK(cursor(a) == 47);
    a.distanceSq = 1.0f;
    CHECK(cursor(a) == 21);
}

TEST_CASE("a service: the quest, the icon, the flags in their order", "[pointer]") {
    CursorAsk a{.distanceSq = 36.0f, .serviceOffered = true, .npcFlags = 0x1 | 0x2 | 0x80 | 0x1000, .combatReach = 2.0f};
    CHECK(cursor(a) == 18);
    a.distanceSq = 37.0f;
    CHECK(cursor(a) == 44);
    a.icon = 0x35;
    a.iconUnable = 0x34;
    CHECK(cursor(a) == 0x34);
    a.distanceSq = 1.0f;
    CHECK(cursor(a) == 0x35);
    for (const auto& [status, kind] : {std::pair{2, 23}, {3, 23}, {4, 23}, {5, 23}, {6, 24}, {7, 23}, {8, 23}, {9, 25}, {10, 25}}) {
        a.questStatus = static_cast<uint8_t>(status);
        CHECK(cursor(a) == kind);
    }
    a.distanceSq = 100.0f;
    CHECK(cursor(a) == 51);
    a.questStatus = 1;
    CHECK(cursor(a) == 0x34);
    // A service goes before an attack; a flag of no service leaves the pointer.
    a.attackable = true;
    CHECK(cursor(a) == 0x34);
    a.icon = a.iconUnable = 0;
    a.npcFlags = 0x2;
    CHECK(cursor(a) == 0);
    a.seatFree = true;
    CHECK(cursor(a) == 52);
    a.serviceOffered = false;
    CHECK(cursor(a) == 30);
    for (const auto& [flags, kind] : {std::pair{0x10000u, 22}, {0x2000u, 9}, {0x10u, 10}, {0x4000u, 6}, {0x8000u, 6}, {0x20000u, 3}, {0x800000u, 3},
                                      {0x40000u, 6}, {0x80000u, 6}, {0x100000u, 6}, {0x200000u, 3}, {0x400000u, 6}, {0x80u, 8}, {0x1u, 6}, {0x1000000u, 5}})
        CHECK(serviceCursor(flags) == kind);
}

TEST_CASE("looking elsewhere or in a seat of no hands, the sword of the unit possessed alone", "[pointer]") {
    CursorAsk a{.distanceSq = 900.0f, .serviceOffered = true, .npcFlags = 1, .attackable = true};
    a.player.seesElsewhere = true;
    CHECK(cursor(a) == 0);
    a.player.possesses = true;
    CHECK(cursor(a) == 4);
    a.alive = false;
    CHECK(cursor(a) == 0);
    a.alive = true;
    a.player = {.mayInteract = false};
    CHECK(cursor(a) == 0);
}

TEST_CASE("a right click on a unit", "[pointer]") {
    ClickAsk a;
    CHECK(click(a) == Click::Voice);
    a.mayAttack = true;
    CHECK(click(a) == Click::Attack);
    a.maySpeak = true;
    CHECK(click(a) == Click::Speak);
    a.alive = false;
    CHECK(click(a) == Click::Dead);
    a.shownDead = true;
    CHECK(click(a) == Click::Speak);
    a.shownDead = false;
    a.playerIsMover = false;
    CHECK(click(a) == Click::Speak);

    SECTION("the dead click nothing") {
        a.playerAlive = false;
        CHECK(click(a) == Click::Nothing);
    }
    SECTION("in a seat of no hands only an attack") {
        a.player.mayInteract = false;
        CHECK(click(a) == Click::Attack);
        a.mayAttack = false;
        CHECK(click(a) == Click::Nothing);
    }
    SECTION("the unit possessed attacks, and does nothing else") {
        a.player.seesElsewhere = true;
        a.alive = true;
        CHECK(click(a) == Click::Nothing);
        a.player.possesses = true;
        CHECK(click(a) == Click::PetAttack);
        a.playerAlive = false;
        CHECK(click(a) == Click::PetAttack);
        a.mayAttack = false;
        CHECK(click(a) == Click::Nothing);
        a.mayAttack = true;
        a.alive = false;
        CHECK(click(a) == Click::Nothing);
    }
}
