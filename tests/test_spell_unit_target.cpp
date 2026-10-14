#include <catch_amalgamated.hpp>

#include "game/spell_unit_target.hpp"

#include <vector>

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

TEST_CASE("a unit leaving view is forgotten unless it is of the group (0x00524350)", "[unit_target]") {
    REQUIRE(forgottenOnLeaving(false, false));
    REQUIRE_FALSE(forgottenOnLeaving(true, false));
    // One of the group the player duels is forgotten all the same (0x0071f5c0).
    REQUIRE(forgottenOnLeaving(true, true));
    REQUIRE(duelOpponents(0x1234, 1, 0x1234, 2));
    REQUIRE_FALSE(duelOpponents(0x1234, 1, 0x1234, 1));
    REQUIRE_FALSE(duelOpponents(0x1234, 1, 0x5678, 2));
    REQUIRE_FALSE(duelOpponents(0x1234, 0, 0x1234, 2));
}

TEST_CASE("a unit turned not selectable is forgotten (0x0073c330)", "[unit_target]") {
    REQUIRE(forgottenOnFlags(0, kFlagNotSelectable));
    REQUIRE_FALSE(forgottenOnFlags(kFlagNotSelectable, kFlagNotSelectable));
    REQUIRE_FALSE(forgottenOnFlags(kFlagNotSelectable, 0));
    REQUIRE_FALSE(forgottenOnFlags(0, kFlagPlayerControlled));
}

TEST_CASE("the last target is the one left, none included (0x00524bf0)", "[unit_target]") {
    // Taking a unit with none before: the last target is none.
    REQUIRE(lastTargetAfter(0, 0x10, 0x20) == 0);
    REQUIRE(lastTargetAfter(0x30, 0x10, 0x20) == 0x30);
    // Clearing the target: the one let go; with none, nothing changes.
    REQUIRE(lastTargetAfter(0x30, 0, 0x20) == 0x30);
    REQUIRE(lastTargetAfter(0, 0, 0x20) == 0x20);
}

namespace {
// A stand-in for 0x0080bc80: the units it takes, each filling all that is
// wanted, or only part of it.
struct Taken {
    bool taken = false;
    uint32_t required = 0;
    uint64_t unit = 0;
};
constexpr uint64_t kPlayer = 1, kPet = 2, kFriend = 3, kEnemy = 4;
auto takes(std::vector<uint64_t> units, uint32_t left = 0) {
    return [units, left](uint64_t guid) {
        for (uint64_t u : units)
            if (u == guid) return Taken{true, left, guid};
        return Taken{};
    };
}
}  // namespace

TEST_CASE("a spell wanting nothing goes at none (0x0080c790)", "[unit_target]") {
    // A self buff, a shout, an area spell: target or none, autoSelfCast or not.
    const auto c = chooseUnit(0, false, kEnemy, false, true, false, kPlayer, kPlayer, takes({kEnemy}));
    REQUIRE(c.what == Choice::AtNone);
    REQUIRE(chooseUnit(0, false, 0, false, false, true, kPlayer, kPlayer, takes({})).what == Choice::AtNone);
}

TEST_CASE("a heal goes at the friend asked, else the player by autoSelfCast (0x0080c790)", "[unit_target]") {
    // At a friend: that friend.
    auto c = chooseUnit(kUnitAlly, false, kFriend, false, true, false, kPlayer, kPlayer, takes({kFriend, kPlayer}));
    REQUIRE(c.what == Choice::AtUnit);
    REQUIRE(c.unit == kFriend);
    // Nothing selected: the player.
    c = chooseUnit(kUnitAlly, false, 0, false, true, true, kPlayer, kPlayer, takes({kPlayer}));
    REQUIRE(c.what == Choice::AtUnit);
    REQUIRE(c.unit == kPlayer);
    // An enemy selected, which the player may not help: the player.
    c = chooseUnit(kUnitAlly, false, kEnemy, false, true, true, kPlayer, kPlayer, takes({kPlayer}));
    REQUIRE(c.unit == kPlayer);
    // autoSelfCast off: no unit, so the cursor holds it (no refusal for a
    // spell wanting no enemy).
    c = chooseUnit(kUnitAlly, false, kEnemy, false, false, true, kPlayer, kPlayer, takes({kPlayer}));
    REQUIRE(c.what == Choice::NoUnit);
    REQUIRE(noUnitFailure(0, 0, kUnitAlly, true, true, true) == 0);
    // A unit the player may help but the spell does not take is not
    // stood in for by the player.
    c = chooseUnit(kUnitAlly, false, kFriend, false, true, false, kPlayer, kPlayer, takes({kPlayer}));
    REQUIRE(c.what == Choice::NoUnit);
}

TEST_CASE("an enemy spell takes the enemy, and with none is refused (0x0080c790, 0x0080cce0)", "[unit_target]") {
    auto c = chooseUnit(kUnitEnemy, false, kEnemy, false, true, true, kPlayer, kPlayer, takes({kEnemy}));
    REQUIRE(c.what == Choice::AtUnit);
    REQUIRE(c.unit == kEnemy);
    // Nothing selected: autoSelfCast tries the player, who does not fill an
    // enemy, and it is refused - no target, BAD_IMPLICIT_TARGETS.
    c = chooseUnit(kUnitEnemy, false, 0, false, true, true, kPlayer, kPlayer, takes({}));
    REQUIRE(c.what == Choice::NoUnit);
    REQUIRE(noUnitFailure(0, 0, kUnitEnemy, true, true, false) == kFailedNoTarget);
    REQUIRE(noUnitFailure(0, 0, kUnitEnemy, true, true, true) == kFailedBadTargets);
}

TEST_CASE("a pet's spell falls back to the player, then the pet (0x0080c790)", "[unit_target]") {
    auto c = chooseUnit(kUnitAlly, false, 0, false, true, true, kPlayer, kPet, takes({kPet}));
    REQUIRE(c.unit == kPet);
    c = chooseUnit(kUnitAlly, false, 0, false, true, true, kPlayer, kPet, takes({kPlayer, kPet}));
    REQUIRE(c.unit == kPlayer);
}

TEST_CASE("the unit asked out of view goes as it is past 100 yards; ImplicitTargetA 16 takes none", "[unit_target]") {
    auto c = chooseUnit(kUnitAlly, false, kFriend, true, true, false, kPlayer, kPlayer, takes({}));
    REQUIRE(c.what == Choice::AtUnit);
    REQUIRE(c.unit == kFriend);
    c = chooseUnit(kUnitAlly, true, kFriend, false, true, true, kPlayer, kPlayer, takes({kFriend, kPlayer}));
    REQUIRE(c.what == Choice::NoUnit);
    // A unit filling part of what is wanted: nothing more is done.
    c = chooseUnit(kUnitAlly | kUnitParty, false, kFriend, false, true, false, kPlayer, kPlayer,
                   takes({kFriend}, kUnitParty));
    REQUIRE(c.what == Choice::Partly);
}

TEST_CASE("a temporary enchantment goes on the weapon 0x0080c790 picks", "[unit_target]") {
    constexpr uint32_t kRockbiter = 1;  // its EffectMiscValue
    HandWeapon main{true, 2, 0, 0, 0};
    HandWeapon off{true, 2, 0, 0, 0};
    // Rockbiter Weapon (8017): no class or subclass asked.
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, main, off) == Hand::Main);
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, HandWeapon{}, off) == Hand::Off);
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, HandWeapon{}, HandWeapon{}) == Hand::None);
    // The main hand enchanted, the off hand not: the off hand.
    main.tempEnchant = 7;
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, main, off) == Hand::Off);
    // Both enchanted: the one carrying this enchantment, else none.
    off.tempEnchant = 9;
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, main, off) == Hand::None);
    off.tempEnchant = kRockbiter;
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, main, off) == Hand::Off);
    main.tempEnchant = kRockbiter;
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, main, off) == Hand::Main);
    // Both carrying it: the one with less left, the main hand on a tie.
    main.remaining = 600000;
    off.remaining = 300000;
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, main, off) == Hand::Off);
    off.remaining = 600000;
    REQUIRE(enchantHand(static_cast<uint32_t>(-1), 0, kRockbiter, main, off) == Hand::Main);
}

TEST_CASE("a temporary enchantment asking for a weapon class passes over what is not one", "[unit_target]") {
    // Flametongue Weapon (8024): EquippedItemClass 2, subclass mask 0x2a5f3.
    const HandWeapon shield{true, 4, 6, 0, 0};
    const HandWeapon axe{true, 2, 0, 0, 0};
    const HandWeapon bow{true, 2, 2, 0, 0};  // subclass 2 is not in the mask
    REQUIRE(enchantHand(2, 0x2a5f3, 5, shield, axe) == Hand::Off);
    REQUIRE(enchantHand(2, 0x2a5f3, 5, bow, HandWeapon{}) == Hand::None);
    REQUIRE(enchantHand(2, 0x2a5f3, 5, axe, shield) == Hand::Main);
}
