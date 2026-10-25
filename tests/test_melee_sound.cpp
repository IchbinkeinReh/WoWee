// A melee swing's sounds (rendering/melee_sound.hpp): what a kept hit plays
// as its attacker's model lands it.
#include <catch_amalgamated.hpp>

#include "rendering/melee_sound.hpp"

namespace ms = wowee::rendering::melee_sound;

namespace {
ms::Hit hit(uint32_t state, int32_t damage, uint32_t info = 0) {
    ms::Hit h;
    h.attacker = 1;
    h.target = 2;
    h.victimState = state;
    h.damage = damage;
    h.hitInfo = info;
    return h;
}
}  // namespace

TEST_CASE("a hit's settling: impact, miss whoosh or nothing", "[melee_sound]") {
    CHECK(ms::settle(hit(ms::kStateHit, 120)) == ms::Settle::Impact);
    CHECK(ms::settle(hit(ms::kStateHit, 0, ms::kHitMiss)) == ms::Settle::Miss);
    CHECK(ms::settle(hit(ms::kStateDodge, 0)) == ms::Settle::Miss);
    CHECK(ms::settle(hit(ms::kStateParry, 0)) == ms::Settle::None);
    CHECK(ms::settle(hit(ms::kStateBlock, 30)) == ms::Settle::None);
    // A melee spell's swing settles only as a hit.
    auto spell = hit(ms::kStateDodge, 0);
    spell.meleeSpellId = 78;
    CHECK(ms::settle(spell) == ms::Settle::None);
}

TEST_CASE("the swing whooshes but for no state, a dodge or an evade", "[melee_sound]") {
    CHECK(ms::swingSounds(hit(ms::kStateHit, 10)));
    CHECK(ms::swingSounds(hit(ms::kStateParry, 0)));
    CHECK_FALSE(ms::swingSounds(hit(ms::kStateDodge, 0)));
    CHECK_FALSE(ms::swingSounds(hit(ms::kStateEvade, 0)));
    CHECK_FALSE(ms::swingSounds(hit(0, 0)));
}

TEST_CASE("the target's part: parry, block, deflect, absorb", "[melee_sound]") {
    CHECK(ms::defend(hit(ms::kStateParry, 0)) == ms::Defend::Parry);
    auto blocked = hit(ms::kStateBlock, 0);
    blocked.blocked = 40;
    CHECK(ms::defend(blocked) == ms::Defend::Block);
    CHECK(ms::defend(hit(ms::kStateDeflect, 0)) == ms::Defend::Deflect);
    CHECK(ms::defend(hit(ms::kStateHit, 50)) == ms::Defend::None);
    CHECK(ms::absorbs(hit(ms::kStateHit, 0, ms::kHitFullAbsorb)));
    CHECK(ms::absorbs(hit(ms::kStateImmune, 0)));
    CHECK_FALSE(ms::absorbs(hit(ms::kStateHit, 50)));
}

TEST_CASE("a no-wound target's hold runs three to five seconds", "[melee_sound]") {
    CHECK(ms::reactionHoldMs(0) == 3000);
    CHECK(ms::reactionHoldMs(0x80000000u) == 4000);
    CHECK(ms::reactionHoldMs(0xFFFFFFFFu) == 4999);
}

TEST_CASE("the impact table by weapon, column and metal", "[melee_sound]") {
    ms::Tables t;
    // WeaponImpactSounds' one-hand axe, metal: Axe1H_ArmorFlesh 171/172 ...
    t.addImpact(0, 1, {171, 173, 175, 3263, 3262, 1002, 1001, 3202, 3206, 3210},
                {172, 174, 176, 3263, 3262, 1002, 1001, 3203, 3207, 3210});
    t.addSwing(1, false, 235);
    t.addSwing(1, true, 236);
    CHECK(t.impact(0, 0, true, false) == 171);
    CHECK(t.impact(0, 2, true, true) == 176);
    CHECK(t.impact(0, 0, false, false) == 0);  // no wooden one-hand axe row here
    CHECK(t.swing(1, true) == 236);
    CHECK(t.swing(3, false) == 0);
    // A sound override wins over the subclass; nothing held is the fist row.
    CHECK(ms::impactSubclass(ms::Item{2, 7, 15, 1}, 13) == 15);
    CHECK(ms::impactSubclass(ms::Item{2, 7, -1, 1}, 13) == 7);
    CHECK(ms::impactSubclass(std::nullopt, 13) == 13);
}

TEST_CASE("the columns a target and a defender's item sound by", "[melee_sound]") {
    CHECK(ms::creatureImpactColumn(0) == 0);
    CHECK(ms::creatureImpactColumn(1) == 8);
    CHECK(ms::creatureImpactColumn(3) == 9);
    CHECK(ms::creatureImpactColumn(4) == 0);
    CHECK(ms::playerImpactColumn(3) == 2);  // plate: metal | 2
    CHECK(ms::playerImpactColumn(5) == 1);  // chain: metal | 4
    CHECK(ms::playerImpactColumn(0) == 0);
    CHECK(ms::defenceColumn(ms::Item{2, 7, -1, 1}, true) == 5);
    CHECK(ms::defenceColumn(ms::Item{4, 6, -1, 2}, false) == 4);
    CHECK(ms::twoHanded(ms::Item{2, 8, -1, 1}));
    CHECK_FALSE(ms::twoHanded(ms::Item{2, 7, -1, 1}));
}
