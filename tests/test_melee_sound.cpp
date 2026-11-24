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

TEST_CASE("a spell's parry and block sound only for one that lands at once", "[melee_sound]") {
    CHECK(ms::spellMissSounds(0.0f, 0));
    CHECK_FALSE(ms::spellMissSounds(24.0f, 0));
    CHECK_FALSE(ms::spellMissSounds(0.0f, ms::kSpellAttrEx2NoMissSound));
    CHECK(ms::spellDefend(ms::kSpellMissParry) == ms::SpellDefend::Parry);
    CHECK(ms::spellDefend(ms::kSpellMissBlock) == ms::SpellDefend::Block);
    CHECK(ms::spellDefend(ms::kSpellMissDodge) == ms::SpellDefend::None);
}

TEST_CASE("the cries of a melee hit", "[melee_sound]") {
    CHECK(ms::customAttackColumn(0x30484124) == 19);
    CHECK(ms::customAttackColumn(0x33484124) == 22);
    CHECK(ms::customAttackColumn(ms::kEventHit) == 0);
    // The attacker exerts itself for a hit with a state, alive and animated.
    CHECK(ms::exertsOnArrival(hit(ms::kStateHit, 10), true));
    CHECK_FALSE(ms::exertsOnArrival(hit(ms::kStateHit, 10), false));
    CHECK_FALSE(ms::exertsOnArrival(hit(ms::kStateHit, 10, ms::kHitNoAnimation), true));
    CHECK_FALSE(ms::exertsOnArrival(hit(0, 0), true));
    CHECK(ms::exertionSlot(hit(ms::kStateHit, 10, ms::kHitCrit)) == 1);
    // The target's injury.
    CHECK(ms::injurySlot(hit(ms::kStateHit, 10, ms::kHitAffectsVictim)) == 2u);
    CHECK(ms::injurySlot(hit(ms::kStateHit, 10, ms::kHitAffectsVictim | ms::kHitCrit)) == 3u);
    CHECK(ms::injurySlot(hit(ms::kStateHit, 10, ms::kHitAffectsVictim | ms::kHitCrushing | ms::kHitCrit)) == 9u);
    CHECK_FALSE(ms::injurySlot(hit(ms::kStateHit, 10)));
    CHECK_FALSE(ms::injurySlot(hit(ms::kStateHit, 0, ms::kHitAffectsVictim | ms::kHitFullAbsorb)));
    CHECK_FALSE(ms::injurySlot(hit(ms::kStateHit, 0, ms::kHitAffectsVictim | ms::kHitMiss)));
}

TEST_CASE("who plays no wound animation", "[melee_sound]") {
    // A vehicle busy with a cast, whatever else it is.
    CHECK(ms::noWound(true, 0u, 0));
    // A creature by its cache's flags, knocked down or not.
    CHECK(ms::noWound(false, ms::kCreatureTypeFlagNoWound, 0));
    CHECK_FALSE(ms::noWound(false, 0u, ms::kAnimKnockdown));
    // Anyone without a cache entry - a player - while knocked down.
    CHECK(ms::noWound(false, std::nullopt, ms::kAnimKnockdown));
    CHECK_FALSE(ms::noWound(false, std::nullopt, 0));
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

// What was blocked, absorbed or resisted of a hit had a text beside its number.
TEST_CASE("a blow is one text over its victim (0x00755a60)", "[melee_landing]") {
    namespace ml = wowee::game::melee_landing;
    using T = wowee::game::CombatTextEntry;
    CHECK(ml::text(hit(1, 30), false) == T::MELEE_DAMAGE);
    CHECK(ml::text(hit(1, 30, ml::kCrit), false) == T::CRIT_DAMAGE);
    auto partly = hit(1, 30, 0x2040);
    partly.blocked = 5;
    CHECK(ml::text(partly, false) == T::MELEE_DAMAGE);
    CHECK(ml::text(hit(5, 30), false) == T::BLOCK);
    CHECK(ml::text(hit(2, 0), true) == T::DODGE);
    CHECK(ml::text(hit(3, 0), false) == T::PARRY);
    CHECK(ml::text(hit(6, 0), false) == T::EVADE);
    CHECK(ml::text(hit(7, 0), false) == T::IMMUNE);
    CHECK(ml::text(hit(8, 0), false) == T::DEFLECT);
    // Nothing done: none of it on the active player.
    CHECK(ml::text(hit(0, 0, 0x10), false) == T::MISS);
    CHECK(ml::text(hit(1, 0, ml::kFullAbsorb), false) == T::ABSORB);
    CHECK(ml::text(hit(1, 0, ml::kFullResist), false) == T::RESIST);
    CHECK_FALSE(ml::text(hit(0, 0, 0x10), true).has_value());
    CHECK_FALSE(ml::text(hit(1, 0, ml::kNoDamageHit), false).has_value());
    // A melee spell's only where it hit.
    auto spell = hit(3, 0);
    spell.meleeSpellId = 78;
    CHECK_FALSE(ml::text(spell, false).has_value());
    CHECK_FALSE(ml::said(spell));
    spell.victimState = 1;
    CHECK(ml::said(spell));
}

TEST_CASE("a victim avoids a blow at $CPP and is not wounded by it", "[melee_landing]") {
    namespace ml = wowee::game::melee_landing;
    CHECK(ml::reaction(hit(3, 0)) == ml::Reaction::Parry);
    CHECK(ml::reaction(hit(2, 0)) == ml::Reaction::Dodge);
    CHECK(ml::reaction(hit(8, 0)) == ml::Reaction::Dodge);
    auto blocked = hit(1, 20);
    blocked.blocked = 12;
    CHECK(ml::reaction(blocked) == ml::Reaction::Block);
    CHECK(ml::reaction(hit(1, 20)) == ml::Reaction::None);
    CHECK(ml::wounds(hit(1, 20, 0x2)));
    CHECK_FALSE(ml::wounds(hit(1, 20)));
    CHECK(ml::bleeds(hit(1, 20, 0x2)));
    CHECK(ml::bleeds(hit(4, 20, 0x2)));
    CHECK_FALSE(ml::bleeds(hit(1, 0, 0x2)));
    CHECK_FALSE(ml::bleeds(hit(3, 20, 0x2)));
    CHECK(ms::isMeleeEvent(ms::kEventReact));
}

// A parry was a wound's animation.
TEST_CASE("a unit parries with what it holds (0x0073b050)", "[melee_landing]") {
    const auto weapon = [](uint32_t subClass) { return ms::Item{ms::kClassWeapon, subClass, -1, 0}; };
    CHECK(ms::parryAnimation(std::nullopt, true, false) == 20u);
    CHECK(ms::parryAnimation(ms::Item{ms::kClassArmor, 6, -1, 0}, true, false) == 20u);
    CHECK(ms::parryAnimation(weapon(7), false, false) == 20u);
    CHECK(ms::parryAnimation(weapon(7), true, false) == 21u);
    CHECK(ms::parryAnimation(weapon(15), true, true) == 21u);
    CHECK(ms::parryAnimation(weapon(8), true, false) == 22u);
    CHECK(ms::parryAnimation(weapon(8), true, true) == 21u);
    CHECK(ms::parryAnimation(weapon(6), true, false) == 23u);
    CHECK(ms::parryAnimation(weapon(13), true, true) == 20u);
    CHECK_FALSE(ms::parryAnimation(weapon(2), true, false).has_value());
}

// A creature or another player swung whatever its model had, a one-handed swing first;
// the player's hands took turns.
TEST_CASE("a unit swings with the weapon of the hand that swings (0x00755130)", "[melee_sound]") {
    const auto weapon = [](uint32_t subClass) { return ms::Item{ms::kClassWeapon, subClass, -1, 0}; };
    const ms::Item shield{ms::kClassArmor, 6, -1, 0};
    CHECK(ms::swingAnimation(false, std::nullopt, std::nullopt) == 16u);
    CHECK(ms::swingAnimation(false, weapon(7), shield) == 17u);
    CHECK(ms::swingAnimation(false, weapon(8), std::nullopt) == 18u);
    CHECK(ms::swingAnimation(false, weapon(8), weapon(8)) == 17u);
    CHECK(ms::swingAnimation(false, weapon(6), std::nullopt) == 19u);
    CHECK(ms::swingAnimation(false, weapon(10), std::nullopt) == 19u);
    CHECK(ms::swingAnimation(false, weapon(15), std::nullopt) == 85u);
    CHECK(ms::swingAnimation(false, weapon(13), weapon(13)) == 16u);
    CHECK(ms::swingAnimation(true, weapon(7), weapon(7)) == 87u);
    CHECK(ms::swingAnimation(true, weapon(7), weapon(15)) == 88u);
    CHECK(ms::swingAnimation(true, std::nullopt, std::nullopt) == 117u);
    CHECK(ms::swingAnimation(true, weapon(7), shield) == 117u);
}
