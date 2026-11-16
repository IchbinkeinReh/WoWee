// The mouseover the client picks while its cursor is off (0x005220b0 through
// 0x00519e80): most nearly ahead of the mover, each kind behind its gate.
#include <catch_amalgamated.hpp>

#include "game/soft_interact.hpp"

namespace si = wowee::game::soft_interact;

namespace {
si::Candidate unitAt(uint64_t guid, float dx, float dy) {
    si::Candidate c;
    c.guid = guid;
    c.kind = si::Kind::Unit;
    c.dx = dx;
    c.dy = dy;
    return c;
}
}  // namespace

TEST_CASE("A service NPC is taken within its reach and four yards", "[soft_interact]") {
    si::Asker a;  // facing 0: along +x
    auto npc = unitAt(1, 5.0f, 0.0f);
    npc.serviceOffered = true;
    npc.npcFlags = 0x1;
    npc.combatReach = 1.5f;
    CHECK(si::pick(a, {npc}) == 1);
    npc.dx = 6.0f;  // past 5.5
    CHECK(si::pick(a, {npc}) == 0);
    npc.dx = 5.0f;
    npc.serviceOffered = false;  // 0x00729620 says no: not taken
    CHECK(si::pick(a, {npc}) == 0);
}

TEST_CASE("Nothing behind, the most nearly ahead wins, a tie the nearer", "[soft_interact]") {
    si::Asker a;
    auto behind = unitAt(1, -3.0f, 0.0f);
    behind.attackable = true;
    CHECK(si::pick(a, {behind}) == 0);
    auto ahead = unitAt(2, 8.0f, 0.0f);
    ahead.attackable = true;
    auto aside = unitAt(3, 3.0f, 3.0f);
    aside.attackable = true;
    CHECK(si::pick(a, {aside, ahead}) == 2);
    auto nearer = unitAt(4, 4.0f, 0.0f);
    nearer.attackable = true;
    CHECK(si::pick(a, {ahead, nearer}) == 4);
    CHECK(si::pick(a, {nearer, ahead}) == 4);
}

TEST_CASE("An enemy within 10.45 yards, only with control and not pacified", "[soft_interact]") {
    si::Asker a;
    auto foe = unitAt(1, 10.4f, 0.0f);
    foe.attackable = true;
    CHECK(si::pick(a, {foe}) == 1);
    foe.dx = 10.5f;
    CHECK(si::pick(a, {foe}) == 0);
    foe.dx = 5.0f;
    a.hasControl = false;
    CHECK(si::pick(a, {foe}) == 0);
    a.hasControl = true;
    a.playerPacified = true;
    CHECK(si::pick(a, {foe}) == 0);
}

TEST_CASE("A lootable corpse at any distance when the player may loot it", "[soft_interact]") {
    si::Asker a;
    auto corpse = unitAt(1, 40.0f, 0.0f);
    corpse.lootableDead = true;
    CHECK(si::pick(a, {corpse}) == 0);
    corpse.playersLootTarget = true;
    CHECK(si::pick(a, {corpse}) == 1);
}

TEST_CASE("Game objects only for the player's own moving", "[soft_interact]") {
    si::Asker a;
    si::Candidate go;
    go.guid = 7;
    go.kind = si::Kind::GameObject;
    go.dx = 30.0f;
    go.goUsable = true;
    CHECK(si::pick(a, {go}) == 7);
    a.playerIsMover = false;
    CHECK(si::pick(a, {go}) == 0);
}

TEST_CASE("A corpse is taken to loot or to skin, by the player's own moving", "[soft_interact]") {
    si::Asker a;
    si::Candidate corpse;
    corpse.guid = 7;
    corpse.kind = si::Kind::Corpse;
    corpse.dx = 30.0f;
    corpse.corpseLootable = true;
    corpse.playerMayLoot = true;
    CHECK(si::pick(a, {corpse}) == 7);
    corpse.playerMayLoot = false;
    CHECK(si::pick(a, {corpse}) == 0);
    corpse.playersLootTarget = true;  // what the player loots now
    CHECK(si::pick(a, {corpse}) == 7);
    a.playerIsMover = false;          // a vehicle moved: none
    CHECK(si::pick(a, {corpse}) == 0);
    a.playerIsMover = true;
    // Skinned within the spell's reach, not a friendly race's.
    si::Candidate skin;
    skin.guid = 8;
    skin.kind = si::Kind::Corpse;
    skin.dx = 4.0f;
    skin.gatherReach = 5.0f;
    CHECK(si::pick(a, {skin}) == 8);
    skin.corpseFriendly = true;
    CHECK(si::pick(a, {skin}) == 0);
    skin.corpseFriendly = false;
    skin.dx = 6.0f;
    CHECK(si::pick(a, {skin}) == 0);
}

TEST_CASE("Looting asks the form, the reach unless autoInteract, and the player's state", "[soft_interact]") {
    si::LootCheck c;
    c.reachSq = 25.0f;
    c.distSq = 24.0f;
    CHECK(si::playerMayLoot(c));
    c.distSq = 26.0f;
    CHECK_FALSE(si::playerMayLoot(c));
    c.anyDistance = true;              // autoInteract, alive, the mover
    CHECK(si::playerMayLoot(c));
    c.formForbids = true;
    CHECK_FALSE(si::playerMayLoot(c));
    c.formForbids = false;
    for (bool si::LootCheck::*f : {&si::LootCheck::looting, &si::LootCheck::deadOrGhost,
                                   &si::LootCheck::falling, &si::LootCheck::stunned}) {
        si::LootCheck d = c;
        d.*f = true;
        CHECK_FALSE(si::playerMayLoot(d));
    }
    c.clientControl = false;
    CHECK_FALSE(si::playerMayLoot(c));
    CHECK(si::lootReach(0.5f, 0.5f) == Catch::Approx(5.0f));
    CHECK(si::lootReach(3.0f, 1.5f) == Catch::Approx(4.5f + 4.0f / 3.0f));
}
