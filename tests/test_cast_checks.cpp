#include <catch_amalgamated.hpp>

#include "game/cast_checks.hpp"
#include "game/outdoors.hpp"

#include <vector>

using namespace wowee::game::cast_check;

// 0x00739650, the client's checks of the player before a spell is cast.

namespace {
Caster idle() {
    Caster c;
    c.mapType = 0;
    c.alive = true;
    c.sheath = 1;
    return c;
}
constexpr uint32_t kInterruptMovement = 0x1;
}  // namespace

TEST_CASE("an ordinary spell from an idle player passes (0x00739650)", "[cast_checks]") {
    Spell s;
    REQUIRE(check(s, idle()).code == 0);
    // Not every state of the world is known: an unknown map, time or area
    // asks nothing of them.
    Caster c = idle();
    c.mapType = -1;
    s.attr = 0x1000 | 0x2000;
    s.areaGroupId = 5;
    REQUIRE(check(s, c).code == 0);
}

TEST_CASE("the history and the power refuse first", "[cast_checks]") {
    Spell s;
    Caster c = idle();
    c.notReady = true;
    c.enoughPower = false;
    REQUIRE(check(s, c).code == kNotReady);
    c.notReady = false;
    REQUIRE(check(s, c).code == kNoPower);
}

TEST_CASE("arena maps refuse long cooldowns; arena spells want one", "[cast_checks]") {
    Spell s;
    s.recoveryTime = 900001;
    Caster c = idle();
    c.mapType = 4;
    REQUIRE(check(s, c).code == kNotInArena);
    s.attrEx4 = 0x20000;
    REQUIRE(check(s, c).code == 0);
    Spell arena;
    arena.attrEx6 = 0x2;
    REQUIRE(check(arena, idle()).code == kOnlyInArena);
}

TEST_CASE("a pet spell wants a pet, a summon none", "[cast_checks]") {
    Spell pet;
    pet.implicitA[0] = 5;
    REQUIRE(check(pet, idle()).code == kNoPet);
    Caster c = idle();
    c.hasSummon = true;
    REQUIRE(check(pet, c).code == 0);
    Spell summon;
    summon.effect[0] = 56;
    REQUIRE(check(summon, c).code == kAlreadyHaveSummon);
    c.hasCharm = true;
    REQUIRE(check(summon, c).code == kAlreadyHaveCharm);
    summon.attrEx = 0x1;  // dismisses the pet
    REQUIRE(check(summon, c).code == 0);
}

TEST_CASE("learning a spell known is a game error, not a failure", "[cast_checks]") {
    Spell s;
    s.effect[1] = 36;
    s.trigger[1] = 1234;
    Caster c = idle();
    c.learnKnown[1] = true;
    const auto r = check(s, c);
    REQUIRE(r.code == 0);
    REQUIRE(r.error == GameError::SpellAlreadyKnown);
    REQUIRE(r.errorSpell == 1234);
}

TEST_CASE("a stun, silence or fear holds the caster unless an aura or the spell lets it", "[cast_checks]") {
    Spell s;
    Caster c = idle();
    c.unitFlags = 0x40000;
    REQUIRE(check(s, c).code == kStunned);
    s.attrEx5 = 0x8;  // usable stunned
    REQUIRE(check(s, c).code == 0);
    s.attrEx5 = 0;
    c.stun.mechanic = 12;
    const auto r = check(s, c);
    REQUIRE(r.code == kPreventedByMechanic);
    REQUIRE(r.arg == 12);
    c.stun.lets = true;
    REQUIRE(check(s, c).code == 0);

    Spell spell;
    spell.preventionType = 1;
    Caster silenced = idle();
    silenced.unitFlags = 0x2000;
    REQUIRE(check(spell, silenced).code == kSilenced);
    spell.preventionType = 2;  // a physical spell is not silenced
    REQUIRE(check(spell, silenced).code == 0);

    Caster feared = idle();
    feared.unitFlags = 0x800000;
    REQUIRE(check(Spell{}, feared).code == kFleeing);
    feared.alive = false;  // the dead are asked none of this
    REQUIRE(check(Spell{}, feared).code == 0);
}

TEST_CASE("an aura is let go by a spell immune to it (0x007fdfa0, 0x00804430)", "[cast_checks]") {
    Spell stun;  // an aura's spell: MOD_STUN, mechanic stun
    stun.aura[0] = 12;
    stun.mechanic = 12;
    Spell trinket;  // breaks stuns: mechanic immunity (77) to it
    trinket.attrEx = 0x8000;
    trinket.effect[0] = 6;
    trinket.aura[0] = 77;
    trinket.misc[0] = 12;
    std::vector<Spell> auras{stun};
    const auto walk = [&](auto&& fn) {
        for (const auto& a : auras)
            if (!fn(a)) return;
    };
    auto h = hold(trinket, walk, {12u});
    REQUIRE(h.lets);
    h = hold(Spell{}, walk, {12u});
    REQUIRE_FALSE(h.lets);
    REQUIRE(h.mechanic == 12);
    // None of the type at all: not let.
    REQUIRE_FALSE(hold(trinket, walk, {7u}).lets);
    // An aura unaffected by invulnerability is not let go.
    auras[0].attr = 0x20000000;
    REQUIRE_FALSE(hold(trinket, walk, {12u}).lets);
}

TEST_CASE("mounted, sitting and sheathed", "[cast_checks]") {
    Spell s;
    Caster c = idle();
    c.mounted = true;
    REQUIRE(check(s, c).code == kNotMounted);
    c.wouldDismount = true;  // autoDismount: it gets down as it casts
    REQUIRE(check(s, c).code == 0);
    c.wouldDismount = false;
    s.attr = 0x1000000;  // castable mounted
    REQUIRE(check(s, c).code == 0);

    Caster sitting = idle();
    sitting.standState = 1;
    REQUIRE(check(Spell{}, sitting).code == kNotStanding);
    sitting.wouldStand = true;
    REQUIRE(check(Spell{}, sitting).code == 0);

    Spell sheathed;
    sheathed.auraInterruptFlags = 0x200;
    Caster c2 = idle();
    c2.sheath = 0;
    REQUIRE(check(sheathed, c2).code == kNotUnsheathed);
}

TEST_CASE("a cast time on the move is refused, an instant spell is not (0x00739650)", "[cast_checks]") {
    Spell s;
    s.interruptFlags = kInterruptMovement;
    Caster c = idle();
    c.moveFlags = 0x1;  // forward
    c.hasCastTime = true;
    REQUIRE(check(s, c).code == kMoving);
    c.hasCastTime = false;
    REQUIRE(check(s, c).code == 0);
    // Falling counts; turning does not.
    c.hasCastTime = true;
    c.moveFlags = 0x1000;
    REQUIRE(check(s, c).code == kMoving);
    c.moveFlags = 0x10;
    REQUIRE(check(s, c).code == 0);
    // A spell at a place is not refused; nor one with AttributesEx2 0x20.
    c.moveFlags = 0x1;
    s.targets = 0x40;
    REQUIRE(check(s, c).code == 0);
    s.targets = 0;
    s.attrEx2 = 0x20;
    REQUIRE(check(s, c).code == 0);
    // A channel broken by moving.
    Spell channel;
    channel.attrEx = 0x4;
    channel.channelInterruptFlags = 0x8;
    Caster m = idle();
    m.moveFlags = 0x4;
    REQUIRE(check(channel, m).code == kMoving);
}

TEST_CASE("forms: one forbidden, one wanted, a stance is no form (0x0072bac0)", "[cast_checks]") {
    Spell notInBear;
    notInBear.stancesNot[0] = 1u << (5 - 1);
    Caster bear = idle();
    bear.form = 5;
    bear.formRow = true;
    REQUIRE(check(notInBear, bear).code == kNotShapeshift);
    bear.wouldUnshift = true;  // autoUnshift: it leaves the form as it casts
    REQUIRE(check(notInBear, bear).code == 0);
    REQUIRE(formFailure(notInBear, bear) == kNotShapeshift);

    Spell catOnly;
    catOnly.stances[0] = 1u << (1 - 1);
    Caster none = idle();
    none.wouldUnshift = true;
    REQUIRE(check(catOnly, none).code == kOnlyShapeshift);
    Caster cat = idle();
    cat.form = 1;
    cat.formRow = true;
    REQUIRE(check(catOnly, cat).code == 0);
    // A spell wanting a form but needing none (AttributesEx2 0x80000):
    // Regrowth out of form.
    catOnly.attrEx2 = 0x80000;
    REQUIRE(check(catOnly, idle()).code == 0);

    // A warrior's stance is no form: a spell for another stance wants one.
    Spell battleOnly;
    battleOnly.stances[0] = 1u << (17 - 1);
    Caster defensive = idle();
    defensive.form = 18;
    defensive.formRow = true;
    defensive.formFlags = 0x1;
    REQUIRE(check(battleOnly, defensive).code == kOnlyShapeshift);
    // Any spell in a form flagged 0x400, without an aura 275.
    Caster flagged = idle();
    flagged.form = 10;
    flagged.formRow = true;
    flagged.formFlags = 0x400;
    REQUIRE(check(Spell{}, flagged).code == kNotShapeshift);
    flagged.ignoresForm = true;
    REQUIRE(check(Spell{}, flagged).code == 0);
}

TEST_CASE("stealth, combat and the caster's aura state", "[cast_checks]") {
    Spell stealthed;
    stealthed.attr = 0x20000;
    REQUIRE(check(stealthed, idle()).code == kOnlyStealthed);
    Caster hidden = idle();
    hidden.visFlags = 0x2;
    REQUIRE(check(stealthed, hidden).code == 0);

    Spell outOfCombat;
    outOfCombat.attr = 0x10000000;
    Caster fighting = idle();
    fighting.unitFlags = 0x80000;
    REQUIRE(check(outOfCombat, fighting).code == kAffectingCombat);
    fighting.ignoresAuraState = true;
    REQUIRE(check(outOfCombat, fighting).code == 0);

    Spell execute;  // wants the caster's aura state 5
    execute.casterAuraState = 5;
    REQUIRE(check(execute, idle()).code == kCasterAurastate);
    Caster ready = idle();
    ready.auraState = 1u << 4;
    REQUIRE(check(execute, ready).code == 0);
    Spell notWhile;
    notWhile.casterAuraStateNot = 5;
    REQUIRE(check(notWhile, ready).code == kCasterAurastate);
    Spell wantsAura;
    wantsAura.casterAuraSpell = 99;
    REQUIRE(check(wantsAura, idle()).code == kCasterAurastate);
}

TEST_CASE("reputation, area, time of day and maps", "[cast_checks]") {
    Spell s;
    s.minFactionId = 1;
    s.minReputation = 5;  // honored
    Caster c = idle();
    c.reputation = 8999;
    REQUIRE(check(s, c).code == kReputation);
    c.reputation = 9000;
    REQUIRE(check(s, c).code == 0);

    Spell area;
    area.areaGroupId = 7;
    Caster away = idle();
    away.area = 2;
    away.areaArg = 4395;
    const auto r = check(area, away);
    REQUIRE(r.code == kRequiresArea);
    REQUIRE(r.arg == 4395);
    away.area = 1;
    REQUIRE(check(area, away).code == kIncorrectArea);
    away.area = 0;
    REQUIRE(check(area, away).code == 0);

    Spell day;
    day.attr = 0x1000;
    Caster night = idle();
    night.daytime = 0;
    REQUIRE(check(day, night).code == kOnlyDaytime);
    Spell nightOnly;
    nightOnly.attr = 0x2000;
    night.daytime = 1;
    REQUIRE(check(nightOnly, night).code == kOnlyNighttime);

    Spell bg;
    bg.attrEx3 = 0x800;
    REQUIRE(check(bg, idle()).code == kOnlyBattlegrounds);
    Spell noRaid;
    noRaid.attrEx6 = 0x800;
    Caster raid = idle();
    raid.mapType = 2;
    REQUIRE(check(noRaid, raid).code == kNotInRaidInstance);

    Spell flying;
    flying.attrEx4 = 0x4000000;
    Caster ground = idle();
    ground.flyable = 0;
    REQUIRE(check(flying, ground).code == kNotHere);
}

TEST_CASE("indoors and outdoors spells are held to where the player stands (0x0071b7f0)", "[cast_checks]") {
    Spell travel;  // Travel Form: Attributes 0x8000
    travel.attr = 0x8000;
    Spell mount = travel;  // a mount: outdoors only, aura 78
    mount.aura[0] = 78;
    Spell inside;
    inside.attr = 0x4000;
    Caster c = idle();
    REQUIRE(check(travel, c).code == 0);  // unknown: nothing asked
    c.outdoors = 0;
    REQUIRE(check(travel, c).code == kOnlyOutdoors);
    REQUIRE(check(mount, c).code == kNoMountsAllowed);
    REQUIRE(check(inside, c).code == 0);
    c.outdoors = 1;
    REQUIRE(check(travel, c).code == 0);
    REQUIRE(check(mount, c).code == 0);
    REQUIRE(check(inside, c).code == kOnlyIndoors);
}

TEST_CASE("indoors is the building group's, else the ground's area (0x0077fbf0)", "[cast_checks]") {
    using namespace wowee::game::outdoors;
    Place p;
    REQUIRE(outdoors(p, 0));                 // no group, an area flagged neither
    REQUIRE_FALSE(outdoors(p, 0x2000000u));  // AREA_FLAG_INSIDE
    REQUIRE(outdoors(p, 0x6000000u));        // outside wins
    p.inGroup = true;
    REQUIRE_FALSE(outdoors(p, 0x4000000u));  // an interior group, whatever the area
    p.groupFlags = 0x8;                      // exterior
    REQUIRE(outdoors(p, 0));
    p.hasRow = true;
    p.rowFlags = 0x2;  // WMOAreaTable: indoors
    REQUIRE_FALSE(outdoors(p, 0));
    p.groupFlags = 0;
    p.rowFlags = 0x4;  // outdoors
    REQUIRE(outdoors(p, 0));
}
