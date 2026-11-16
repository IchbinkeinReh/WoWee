#include <catch_amalgamated.hpp>

#include "game/world_pick.hpp"

using namespace wowee::game::world_pick;

// What the world frame's ray pick may find (0x004f7650's flags, 0x004f7530's
// filter with 0x004f7350 and 0x007ffea0, 0x004f9930's answer).

TEST_CASE("No spell held: every object kind once there is a player", "[world_pick]") {
    FlagsInput in;
    CHECK(pickFlags(in) == 0u);
    in.activePlayer = true;
    CHECK(pickFlags(in) == 0x5cu);
    // Click to move adds the ground, and the water while walking on it.
    in.moverClickToMove = true;
    CHECK(pickFlags(in) == 0x5du);
    in.moverMoveFlags = kMoveWaterWalking;
    CHECK(pickFlags(in) == 0x5fu);
    in.moverMoveFlags = kMoveWaterWalking | kMoveSwimming;
    CHECK(pickFlags(in) == 0x5du);
    // ...not while a click-to-move is under way.
    in.clickToMoveIdle = false;
    CHECK(pickFlags(in) == 0x5cu);
}

TEST_CASE("A spell held picks only what it wants", "[world_pick]") {
    FlagsInput in;
    in.activePlayer = true;
    in.spellHeld = true;
    in.targetMask = 0x40;  // a place
    CHECK(pickFlags(in) == (kGround | kLiquid));
    in.targetMask = 0x800;  // an object
    CHECK(pickFlags(in) == kGameObjects);
    in.targetMask = 0x80;  // an enemy
    CHECK(pickFlags(in) == (kUnits | kPlayers | kHostile | kAlive));
    in.targetMask = 0x100;  // an ally, the caster among them
    CHECK(pickFlags(in) == (kUnits | kPlayers | kSelf | kFriendly | kAlive));
    in.attrEx = 0x80000;  // never the caster
    CHECK(pickFlags(in) == (kUnits | kPlayers | kFriendly | kAlive));
    in.targetMask = 0x400;  // the dead
    in.attrEx = 0;
    CHECK(pickFlags(in) ==
          (kUnits | kPlayers | kSelf | kParty | kRaid | kFriendly | kHostile | kDead | kCorpses));
    in.targetMask = 0x2;
    in.attrEx2 = 0x1;  // the dead besides the living
    CHECK(pickFlags(in) == (kUnits | kPlayers | kSelf | kDead | kCorpses | kAlive));
    in.targetMask = 0x8000;  // an ally's corpse
    in.attrEx2 = 0;
    CHECK(pickFlags(in) ==
          (kUnits | kPlayers | kSelf | kDead | kCorpses | kFriendly));
}

TEST_CASE("A unit: never the player nor what it possesses without kSelf", "[world_pick]") {
    UnitInput u;
    u.health = 10;
    CHECK(unitPickable(0x5c, u));
    u.isActivePlayer = true;
    CHECK_FALSE(unitPickable(0x5c, u));
    CHECK(unitPickable(0x5c | kSelf, u));
    u.isActivePlayer = false;
    u.possessedByActivePlayer = true;
    CHECK_FALSE(unitPickable(0x5c, u));
    CHECK(unitPickable(0x5c | kSelf, u));
}

TEST_CASE("A unit: the relation, life and immunity a held spell asks", "[world_pick]") {
    UnitInput u;
    u.health = 10;
    u.spellHeld = true;
    const uint32_t enemy = kUnits | kPlayers | kHostile | kAlive;
    CHECK_FALSE(unitPickable(enemy, u));
    u.mayAttack = true;
    CHECK(unitPickable(enemy, u));
    u.health = 0;
    CHECK_FALSE(unitPickable(enemy, u));
    u.health = 10;
    u.unitFlags = kUnitFlagImmuneToPc;
    CHECK_FALSE(unitPickable(enemy, u));
    u.attrEx3 = 0x10000000;
    CHECK(unitPickable(enemy, u));
    // An ally wanted: a unit that is not one only as a charmed enemy.
    const uint32_t ally = kUnits | kPlayers | kFriendly | kAlive;
    UnitInput c;
    c.health = 5;
    c.mayAttack = true;
    c.charmed = true;
    CHECK_FALSE(unitPickable(ally, c));
    CHECK(unitPickable(ally | kCharmedEnemy, c));
    c.mayAssist = true;
    CHECK(unitPickable(ally, c));
    // With no spell held the immunity does not matter.
    UnitInput n;
    n.health = 1;
    n.unitFlags = kUnitFlagImmuneToPc;
    CHECK(unitPickable(0x5c, n));
    // A companion wherever one is wanted.
    UnitInput pet;
    pet.creatureType = kCreatureTypeCompanion;
    pet.spellHeld = true;
    pet.unitFlags = kUnitFlagImmuneToPc;
    CHECK(unitPickable(kUnits | kCompanion, pet));
}

TEST_CASE("Objects and corpses", "[world_pick]") {
    CHECK(gameObjectPickable(0x5c, false, false));
    CHECK_FALSE(gameObjectPickable(kUnits, false, false));
    CHECK_FALSE(gameObjectPickable(kGameObjects, true, false));
    CHECK(gameObjectPickable(kGameObjects, true, true));

    CorpseInput c;
    CHECK(corpsePickable(0x5c, c));
    CHECK_FALSE(corpsePickable(kUnits, c));
    c.spellHeld = true;
    c.targetMask = kTargetCorpseAlly;
    CHECK_FALSE(corpsePickable(kCorpses, c));
    c.friendly = true;
    CHECK(corpsePickable(kCorpses, c));
    c.bones = true;
    CHECK_FALSE(corpsePickable(kCorpses, c));
    c.bones = false;
    c.targetMask = kTargetCorpseEnemy;
    CHECK_FALSE(corpsePickable(kCorpses, c));
    c.friendly = false;
    CHECK(corpsePickable(kCorpses, c));
}

TEST_CASE("The ray pick's answer", "[world_pick]") {
    CHECK(resolveHit(0x5c, true, true, false) == Hit::Object);
    CHECK(resolveHit(0x5c, false, true, true) == Hit::WorldObject);
    CHECK(resolveHit(0x5c, false, true, false) == Hit::None);
    CHECK(resolveHit(0x5d, false, true, false) == Hit::Ground);
    CHECK(resolveHit(0x5d, false, false, false) == Hit::None);
}
