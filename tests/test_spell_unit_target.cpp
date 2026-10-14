#include <catch_amalgamated.hpp>

#include "game/spell_unit_target.hpp"

using namespace wowee::game::unit_target;

// What a spell takes of a unit (0x0080bc80), the relations it asks of its
// caster (0x007293d0, 0x00729740), the reach (0x007ff480, 0x00803850), and
// 0x0080cce0's answer when no unit fills the spell.

namespace {
Side pet() {
    Side s;
    s.unitFlags = kFlagPlayerControlled;
    return s;
}
Unit creature(int32_t health = 100) {
    Unit u;
    u.type = kTypeUnit;
    u.health = health;
    return u;
}
}  // namespace

TEST_CASE("a pet may help a friendly creature only where it is flagged for it (0x007293d0)", "[unit_target]") {
    Side npc;
    REQUIRE_FALSE(mayAssist(pet(), npc, kReactionFriendly, false, {}));
    npc.creatureTypeFlags = 0x1000;  // CAN_ASSIST
    REQUIRE(mayAssist(pet(), npc, kReactionFriendly, false, {}));
    // Not one it is unfriendly with, nor one immune to players.
    REQUIRE_FALSE(mayAssist(pet(), npc, 3, false, {}));
    npc.unitFlags = kFlagImmuneToPc;
    REQUIRE_FALSE(mayAssist(pet(), npc, kReactionFriendly, false, {}));
    // The last argument passes over both.
    Side plain;
    REQUIRE(mayAssist(pet(), plain, kReactionFriendly, true, {}));
    plain.pvpFlags = 1;  // flagged for PvP
    REQUIRE(mayAssist(pet(), plain, kReactionFriendly, false, {}));
}

TEST_CASE("a pet helps a player in a duel only from the same side (0x007293d0)", "[unit_target]") {
    Side player = pet();
    player.isPlayer = true;
    Controllers c{true, 7, 7, 1, 1};
    REQUIRE(mayAssist(pet(), player, kReactionFriendly, false, c));
    c.casterTeam = 2;
    REQUIRE_FALSE(mayAssist(pet(), player, kReactionFriendly, false, c));
    // Free-for-all: not into it from outside.
    c = {};
    player.pvpFlags = 4;
    REQUIRE_FALSE(mayAssist(pet(), player, kReactionFriendly, false, c));
}

TEST_CASE("a pet attacks what is not friendly and not out of reach (0x00729740)", "[unit_target]") {
    Side npc;
    REQUIRE(mayAttack(pet(), npc, 3, 3, {}));
    REQUIRE_FALSE(mayAttack(pet(), npc, kReactionFriendly, 3, {}));
    npc.unitFlags = 0x2;  // non-attackable
    REQUIRE_FALSE(mayAttack(pet(), npc, 1, 1, {}));
    npc.unitFlags = kFlagImmuneToPc;
    REQUIRE_FALSE(mayAttack(pet(), npc, 1, 1, {}));
    npc.unitFlags = 0;
    npc.pvpFlags = 8;  // in a sanctuary
    REQUIRE_FALSE(mayAttack(pet(), npc, 1, 1, {}));
    // Two players' sides: by the duel, else the PvP flags.
    Side other = pet();
    other.isPlayer = true;
    REQUIRE_FALSE(mayAttack(pet(), other, 1, 1, {true, 0, 0, 0, 0}));
    REQUIRE(mayAttack(pet(), other, 1, 1, {true, 5, 5, 1, 2}));
    other.pvpFlags = 1;
    REQUIRE(mayAttack(pet(), other, 1, 1, {true, 0, 0, 0, 0}));
    // A ghost only for one that sees them.
    other.playerFlags = 0x10;
    REQUIRE_FALSE(mayAttack(pet(), other, 1, 1, {true, 0, 0, 0, 0}));
}

TEST_CASE("a unit fills what the spell wants in the client's order (0x0080bc80)", "[unit_target]") {
    const Spell s;
    Relations r;
    r.mayAttack = true;
    // An enemy wanted: taken, and nothing more is.
    auto t = takeUnit(s, creature(), r, kUnitEnemy);
    REQUIRE(t.taken);
    REQUIRE(t.required == 0);
    REQUIRE(t.mask == kUnit);
    REQUIRE(reachChecked(t.mask));
    // Not one it may not attack.
    r.mayAttack = false;
    REQUIRE_FALSE(takeUnit(s, creature(), r, kUnitEnemy).taken);
    // An ally: by 0x007293d0 with AttributesEx6 0x8.
    r.mayAssistEx6 = true;
    REQUIRE(takeUnit(s, creature(), r, kUnitAlly).taken);
    // Any unit, but one immune to players without the attributes for it.
    Unit immune = creature();
    immune.unitFlags = kFlagImmuneToPc;
    REQUIRE_FALSE(takeUnit(s, immune, {}, kUnit).taken);
    Spell ex3;
    ex3.attrEx3 = 0x10000000;
    REQUIRE(takeUnit(ex3, immune, {}, kUnit).taken);
    // A party member wants both the party and help.
    r = {};
    r.inParty = true;
    REQUIRE_FALSE(takeUnit(s, creature(), r, kUnitParty).taken);
    r.mayAssist = true;
    REQUIRE(takeUnit(s, creature(), r, kUnitParty).taken);
}

TEST_CASE("the dead, the caster and the creature types (0x0080bc80)", "[unit_target]") {
    Relations r;
    r.mayAttack = true;
    const Spell s;
    // The dead only where the dead are wanted.
    REQUIRE_FALSE(takeUnit(s, creature(0), r, kUnitEnemy).taken);
    const auto t = takeUnit(s, creature(0), r, kUnitEnemy | kUnitDead);
    REQUIRE(t.taken);
    REQUIRE(t.required == 0);
    Spell atDead;
    atDead.attrEx2 = 0x1;
    REQUIRE(takeUnit(atDead, creature(0), r, kUnitEnemy).taken);
    // The living not where only the dead are.
    REQUIRE_FALSE(takeUnit(s, creature(), r, kUnitEnemy | kUnitDead).taken);
    // A dead player's corpse for an ally's, where it may be helped.
    Unit corpse = creature(0);
    corpse.type = kTypeUnit | kTypePlayer;
    r = {};
    REQUIRE_FALSE(takeUnit(s, corpse, r, kCorpseAlly).taken);
    r.mayAssist = true;
    REQUIRE(takeUnit(s, corpse, r, kCorpseAlly).taken);
    // Not the caster with AttributesEx 0x80000.
    Spell notSelf;
    notSelf.attrEx = 0x80000;
    Unit self = creature();
    self.isCaster = true;
    REQUIRE_FALSE(takeUnit(notSelf, self, {}, kUnit).taken);
    // TargetCreatureType: a beast (1) for a mask naming beasts.
    Spell beasts;
    beasts.targetCreatureType = 0x1;
    Unit beast = creature();
    beast.creatureType = 1;
    REQUIRE(takeUnit(beasts, beast, {}, kUnit).taken);
    beast.creatureType = 7;
    REQUIRE_FALSE(takeUnit(beasts, beast, {}, kUnit).taken);
    // A companion, by its type.
    Unit minipet = creature();
    minipet.creatureType = 12;
    const auto m = takeUnit(s, minipet, {}, kUnitMinipet);
    REQUIRE(m.taken);
    REQUIRE(m.mask == kUnitMinipet);
    // A game object for a game object.
    Unit go;
    go.type = kTypeGameObject;
    REQUIRE(takeUnit(s, go, {}, kGameObject).taken);
    REQUIRE_FALSE(takeUnit(s, go, {}, kUnit).taken);
}

TEST_CASE("the reach to a unit (0x007ff480) and its refusals (0x00803850)", "[unit_target]") {
    const auto none = [](float m) { return m; };
    RangeInput in;
    in.hasRow = true;
    in.rowMin = 0.0f;
    in.rowMax = 30.0f;
    in.casterReach = 1.5f;
    in.hasUnit = true;
    in.unitReach = 1.5f;
    auto r = unitRange(in, none);
    REQUIRE(r.min == 0.0f);
    REQUIRE(r.max == Catch::Approx(33.0f));
    REQUIRE(reachFailure(30.0f * 30.0f, r) == 0);
    REQUIRE(reachFailure(40.0f * 40.0f, r) == kFailedOutOfRange);
    // Melee: both reaches and 4/3, at least 5.
    in.rowFlags = 1;
    r = unitRange(in, none);
    REQUIRE(r.max == Catch::Approx(5.0f));
    // Ranged: the minimum starts there.
    in.rowFlags = 2;
    in.rowMin = 5.0f;
    r = unitRange(in, none);
    REQUIRE(r.min == Catch::Approx(10.0f));
    REQUIRE(reachFailure(4.0f, r) == kFailedTooClose);
    // Attributes 0x404: 0 to 100.
    in.attr = 0x4;
    r = unitRange(in, none);
    REQUIRE(r.min == 0.0f);
    REQUIRE(r.max == 100.0f);
}

TEST_CASE("the unit cast at is held to the reach as what it may be taken as (0x00809610)", "[unit_target]") {
    // Charge: TARGET_UNIT_TARGET_ENEMY, an enemy, nothing of its own.
    auto k = reachKinds(0, {6, 0, 0});
    REQUIRE(k.mask == kUnitEnemy);
    REQUIRE_FALSE(k.pet);
    // A friendly spell; a dead unit's bit taken away by the caster (1).
    k = reachKinds(kUnitDead | kUnitAlly, {21, 1, 0});
    REQUIRE(k.mask == kUnitAlly);
    // The pet stands for the unit where no plain unit is wanted.
    k = reachKinds(kCorpseAlly, {5, 0, 0});
    REQUIRE(k.pet);
    REQUIRE(k.mask == kUnit);
    k = reachKinds(0, {5, 25, 0});
    REQUIRE_FALSE(k.pet);
    REQUIRE(k.mask == kUnit);
    // Charge's SpellRange row, 8 to 25 and neither melee nor ranged: both
    // reaches widen the minimum as well as the maximum.
    RangeInput in;
    in.hasRow = true;
    in.rowMin = 8.0f;
    in.rowMax = 25.0f;
    in.casterReach = 1.5f;
    in.hasUnit = true;
    in.unitReach = 1.5f;
    const auto r = unitRange(in, [](float m) { return m; });
    REQUIRE(r.min == Catch::Approx(11.0f));
    REQUIRE(r.max == Catch::Approx(28.0f));
    REQUIRE(reachFailure(10.0f * 10.0f, r) == kFailedTooClose);
    REQUIRE(reachFailure(20.0f * 20.0f, r) == 0);
    REQUIRE(reachFailure(29.0f * 29.0f, r) == kFailedOutOfRange);
}

TEST_CASE("may attack now: a caster able to fight, a unit alive (0x00729a70)", "[unit_target]") {
    REQUIRE(mayAttackNow(false, false, false, 100, 0, true));
    REQUIRE_FALSE(mayAttackNow(false, false, false, 100, 0, false));
    // Dead, or mounted with no dismounting for it.
    REQUIRE_FALSE(mayAttackNow(true, false, false, 100, 0, true));
    REQUIRE_FALSE(mayAttackNow(false, true, false, 100, 0, true));
    REQUIRE(mayAttackNow(false, true, true, 100, 0, true));
    // A unit with no health counts only while flagged dead.
    REQUIRE_FALSE(mayAttackNow(false, false, false, 0, 0, true));
    REQUIRE(mayAttackNow(false, false, false, 0, 0x20, true));
    // 0x0071b0c0: autoDismount, never on a taxi, autoDismountFlying in the air.
    REQUIRE(wouldDismount(0, false, true, false));
    REQUIRE_FALSE(wouldDismount(0, false, false, true));
    REQUIRE_FALSE(wouldDismount(0x100000, false, true, true));
    REQUIRE_FALSE(wouldDismount(0, true, true, false));
    REQUIRE(wouldDismount(0, true, true, true));
}

TEST_CASE("no unit: the cursor, or the refusals 0x32, 0xc and 0xb (0x0080cce0)", "[unit_target]") {
    // A friendly spell waits for a click.
    REQUIRE(noUnitFailure(0, 0, kUnitAlly, false, false, true) == 0);
    // An enemy wanted: bad targets with one selected, else no target.
    REQUIRE(noUnitFailure(0, 0, kUnitEnemy, false, false, true) == kFailedBadTargets);
    REQUIRE(noUnitFailure(0, 0, kUnitEnemy, false, false, false) == kFailedNoTarget);
    // AttributesEx5 0x800 lets an ally stand for it.
    REQUIRE(noUnitFailure(0, 0x800, kUnitEnemy | kUnitAlly, false, false, true) == 0);
    // Attributes 0x200's main hand, never a pet's.
    REQUIRE(noUnitFailure(0x200, 0, kUnitAlly, false, true, true) == kFailedMainhand);
    REQUIRE(noUnitFailure(0x200, 0, kUnitAlly, true, true, true) == 0);
}

TEST_CASE("AttributesEx5 0x800 lets a unit stand for its own target (0x007ffc40)", "[unit_target]") {
    Relations ally;
    ally.mayAssist = ally.mayAssistEx6 = true;
    Relations enemy;
    enemy.mayAttack = true;
    // An enemy wanted: an ally gives its target if that may be attacked.
    REQUIRE(retarget(kUnitEnemy, ally, true, enemy) == Retarget::ItsTarget);
    REQUIRE(retarget(kUnitEnemy, ally, true, ally) == Retarget::None);
    REQUIRE(retarget(kUnitEnemy, ally, false, {}) == Retarget::None);
    REQUIRE(retarget(kUnitEnemy, enemy, true, ally) == Retarget::Itself);
    // An ally wanted: an enemy gives its target if that may be helped.
    REQUIRE(retarget(kUnitAlly, enemy, true, ally) == Retarget::ItsTarget);
    REQUIRE(retarget(kUnitAlly, enemy, true, enemy) == Retarget::None);
    REQUIRE(retarget(kUnitAlly, ally, true, enemy) == Retarget::Itself);
    // A party or raid member wanted: only one in the party or raid.
    REQUIRE(retarget(kUnitParty, enemy, true, ally) == Retarget::None);
    Relations member = ally;
    member.inParty = true;
    REQUIRE(retarget(kUnitParty, enemy, true, member) == Retarget::ItsTarget);
    REQUIRE(retarget(kUnitRaid, enemy, true, member) == Retarget::None);
    member.inRaid = true;
    REQUIRE(retarget(kUnitRaid, enemy, true, member) == Retarget::ItsTarget);
    // Neither side wanted: itself.
    REQUIRE(retarget(kUnit, enemy, true, ally) == Retarget::Itself);
    // An ally wanted drops an enemy wanted; otherwise nothing changes.
    REQUIRE(retargetRequired(kUnitAlly | kUnitEnemy) == kUnitAlly);
    REQUIRE(retargetRequired(kUnitEnemy | kUnit) == (kUnitEnemy | kUnit));
}

TEST_CASE("both moving, a melee row or a player unit reaches 8/3 more (0x007ff480)", "[unit_target]") {
    const auto same = [](float m) { return m; };
    RangeInput melee;
    melee.hasRow = true;
    melee.rowFlags = 1;
    melee.casterReach = 1.5f;
    melee.hasUnit = true;
    melee.unitReach = 1.5f;
    const float still = unitRange(melee, same).max;
    REQUIRE(still == Catch::Approx(5.0f));
    melee.casterMoving = true;
    REQUIRE(unitRange(melee, same).max == Catch::Approx(still));
    melee.unitMoving = true;
    REQUIRE(unitRange(melee, same).max == Catch::Approx(still + 8.0f / 3.0f));
    // A ranged row only at a player.
    RangeInput ranged;
    ranged.hasRow = true;
    ranged.rowMax = 30.0f;
    ranged.hasUnit = true;
    ranged.casterMoving = ranged.unitMoving = true;
    REQUIRE(unitRange(ranged, same).max == Catch::Approx(30.0f));
    ranged.unitIsPlayer = true;
    REQUIRE(unitRange(ranged, same).max == Catch::Approx(30.0f + 8.0f / 3.0f));
    // No row, nothing more.
    RangeInput none;
    none.hasUnit = true;
    none.unitIsPlayer = none.casterMoving = none.unitMoving = true;
    REQUIRE(unitRange(none, same).max == Catch::Approx(0.0f));
}
