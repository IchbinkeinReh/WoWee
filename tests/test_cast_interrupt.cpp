// Whether a unit's cast shows as one the player cannot interrupt: the masks
// of the player's spells (0x0053ca70), the judgement against the caster's
// immunities (0x007262e0) and what the cast reports (0x0071ab20).
#include <catch_amalgamated.hpp>
#include "game/cast_interrupt.hpp"

using namespace wowee::game::cast_interrupt;

namespace {
SpellFields kick() {  // effect 68 (interrupt cast), physical
    SpellFields s;
    s.effect[0] = 0x44;
    s.schoolMask = 0x1;
    return s;
}
SpellFields silence() {  // effect 6 applying aura 27, shadow
    SpellFields s;
    s.effect[1] = 6;
    s.auraName[1] = 0x1b;
    s.schoolMask = 0x20;
    return s;
}
SpellFields bossCast(uint32_t preventionType = 1) {
    SpellFields s;
    s.preventionType = preventionType;
    s.schoolMask = 0x4;
    return s;
}
Aura aura(uint32_t auraName, uint32_t misc, uint8_t effectMask = 1) {
    Aura a;
    a.spell.auraName[0] = auraName;
    a.spell.miscValue[0] = misc;
    a.effectMask = effectMask;
    return a;
}
}  // namespace

TEST_CASE("the player's spells build the interrupt and silence masks", "[cast_interrupt]") {
    Masks m;
    addKnownSpell(m, kick());
    addKnownSpell(m, silence());
    SpellFields npOnly;
    npOnly.attrEx7 = 0x800 | 0x1000;
    npOnly.schoolMask = 0x40;
    addKnownSpell(m, npOnly);
    CHECK(m.interrupt == 0x1);
    CHECK(m.silence == 0x20);
    CHECK(m.interruptNonPlayer == 0x40);
    CHECK(m.silenceNonPlayer == 0x40);
    // Effect 6 with another aura is no silence.
    Masks other;
    SpellFields buff;
    buff.effect[0] = 6;
    buff.auraName[0] = 3;
    buff.schoolMask = 0x2;
    addKnownSpell(other, buff);
    CHECK_FALSE(other.any(false));
    // The non-player masks do not count against a player.
    Masks np;
    addKnownSpell(np, npOnly);
    CHECK(np.any(false));
    CHECK_FALSE(np.any(true));
}

TEST_CASE("a cast the player's interrupt reaches stays interruptible", "[cast_interrupt]") {
    Masks m;
    addKnownSpell(m, kick());
    const Outcome o = evaluate(m, true, bossCast(), Caster{});
    CHECK_FALSE(o.lock);
    CHECK_FALSE(o.result);
    CHECK_FALSE(notInterruptible(o.lock, m, false));
}

TEST_CASE("a cast no silence prevents is not interruptible", "[cast_interrupt]") {
    Masks m;
    addKnownSpell(m, kick());
    const Outcome o = evaluate(m, true, bossCast(0), Caster{});
    CHECK(o.lock);
    CHECK(o.result);
    CHECK(notInterruptible(o.lock, m, false));
    // Nor with no player object.
    CHECK(evaluate(m, false, bossCast(), Caster{}).lock);
}

TEST_CASE("AttributesEx7 0x2000 is always interruptible", "[cast_interrupt]") {
    Masks m;
    addKnownSpell(m, kick());
    SpellFields s = bossCast(0);
    s.attrEx7 = 0x2000;
    const Outcome o = evaluate(m, true, s, Caster{});
    CHECK_FALSE(o.lock);
    CHECK_FALSE(o.result);
}

TEST_CASE("knowing no interrupt leaves the bit clear but answers 1", "[cast_interrupt]") {
    const Masks none;
    const Outcome o = evaluate(none, true, bossCast(), Caster{});
    CHECK_FALSE(o.lock);
    CHECK(o.result);
    // And whatever the bit, nothing is reported.
    CHECK_FALSE(notInterruptible(true, none, false));
}

TEST_CASE("the caster's immunities from the cast take the interrupts away", "[cast_interrupt]") {
    Masks m;
    addKnownSpell(m, kick());
    addKnownSpell(m, silence());
    Caster c;
    c.mechanicImmunity = 0x4000000;  // interrupt (26)
    CHECK_FALSE(evaluate(m, true, bossCast(), c).lock);  // the silence still reaches
    c.mechanicImmunity |= 0x200;      // silence (9)
    CHECK(evaluate(m, true, bossCast(), c).lock);
    Caster schools;
    schools.schoolImmunity = 0x21;
    CHECK(evaluate(m, true, bossCast(), schools).lock);
}

TEST_CASE("immunity auras on the caster take the interrupts away", "[cast_interrupt]") {
    Masks m;
    addKnownSpell(m, kick());
    addKnownSpell(m, silence());
    Caster c;
    c.auras.push_back(aura(0x25, 0x44));  // effect immunity: interrupt cast
    CHECK_FALSE(evaluate(m, true, bossCast(), c).lock);
    c.auras.push_back(aura(0x4d, 9));  // mechanic immunity: silence
    CHECK(evaluate(m, true, bossCast(), c).lock);

    Caster state;
    state.auras.push_back(aura(0x26, 0x1b));  // state immunity: silence
    state.auras.push_back(aura(0x4d, 0x1a));  // mechanic immunity: interrupt
    CHECK(evaluate(m, true, bossCast(), state).lock);

    Caster school;
    school.auras.push_back(aura(0x27, 0x21));  // school immunity: physical, shadow
    CHECK(evaluate(m, true, bossCast(), school).lock);
}

TEST_CASE("an immunity aura's other effects count only with one in force", "[cast_interrupt]") {
    Masks m;
    addKnownSpell(m, kick());
    // The effect immunity is the aura's second effect; only the first is in force.
    Aura a;
    a.spell.auraName[0] = 3;
    a.spell.auraName[1] = 0x25;
    a.spell.miscValue[1] = 0x44;
    a.effectMask = 0x1;
    Caster c;
    c.auras.push_back(a);
    CHECK_FALSE(evaluate(m, true, bossCast(), c).lock);
    // Another aura with an immunity in force lets every effect count.
    c.auras.push_back(aura(0x27, 0x40));
    CHECK(evaluate(m, true, bossCast(), c).lock);
}

TEST_CASE("the non-player masks are left out against a player caster", "[cast_interrupt]") {
    Masks m;
    SpellFields np;
    np.attrEx7 = 0x800;
    np.schoolMask = 0x1;
    addKnownSpell(m, np);
    Caster npc;
    CHECK_FALSE(evaluate(m, true, bossCast(), npc).lock);
    Caster player;
    player.isPlayer = true;
    // Nothing counts against a player: the bit stays clear, the answer is 1.
    const Outcome o = evaluate(m, true, bossCast(), player);
    CHECK_FALSE(o.lock);
    CHECK(o.result);
}
