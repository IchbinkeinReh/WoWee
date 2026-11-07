#include <catch_amalgamated.hpp>

#include "game/game_object_use.hpp"

#include <vector>

using namespace wowee::game::go_use;

// GameObject_C.cpp's use of an object: 0x0070ef90, 0x0070f160, 0x007116c0.

namespace {

// A herb's lock: LockType 2 (herbalism), skill 70, action 0.
LockRow herb(uint32_t skill) {
    LockRow r;
    r.type[0] = 2;
    r.index[0] = 2;
    r.skill[0] = skill;
    return r;
}

// Herb Gathering: OPEN_LOCK for LockType 2.
bool herbEffects(uint32_t id, uint32_t (&e)[3], uint32_t (&m)[3]) {
    if (id != 2366) return false;
    e[0] = kEffectOpenLock;
    m[0] = 2;
    return true;
}

}  // namespace

TEST_CASE("go_use: a slot's action against the object's state", "[go_use]") {
    CHECK(actionFits(0, 1, 0));
    CHECK_FALSE(actionFits(0, 1, kFlagLocked));
    CHECK(actionFits(1, 1, kFlagLocked));
    CHECK_FALSE(actionFits(0, 0, 0));   // 0 wants state 1
    CHECK(actionFits(2, 0, 0));
    CHECK_FALSE(actionFits(2, 2, 0));
    CHECK(actionFits(4, 2, 0));          // only 4 is done in state 2
    CHECK_FALSE(actionFits(4, 1, 0));
}

TEST_CASE("go_use: the lock field by type", "[go_use]") {
    CHECK(dataIndex(0, Field::Lock) == 1);   // door
    CHECK(dataIndex(3, Field::Lock) == 0);   // chest
    CHECK(dataIndex(25, Field::Lock) == 4);  // fishing hole
    CHECK(dataIndex(19, Field::Lock) == -1); // a mailbox has none
    CHECK(handlerFor(0).lockedError == err::kDoorLocked);
    CHECK(handlerFor(17).reach == 100.0f);
}

TEST_CASE("go_use: a known spell opens the lock when strong enough", "[go_use]") {
    const std::vector<uint32_t> known = {133, 2366};
    const auto value = [](uint32_t, int) { return 75; };
    const auto none = [](uint32_t) { return false; };
    const auto noKey = [](uint32_t) { return 0u; };
    auto o = evaluate(herb(70), 1, 0, 10, known, 0, herbEffects, value, none, noKey);
    CHECK_FALSE(o.locked);
    CHECK(o.spell == 2366);
    CHECK(o.slot == 0);

    // Too weak: locked, with the spell kept for ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI.
    o = evaluate(herb(100), 1, 0, 10, known, 0, herbEffects, value, none, noKey);
    CHECK(o.locked);
    CHECK(o.spell == 2366);
    CHECK(o.required == 100);
    CHECK(lockedRefusal(herb(100), 3, 1, 0, o.spell != 0) == err::kLockedWithSpellKnown);

    // No skill on the slot: the object's level times five.
    o = evaluate(herb(0), 1, 0, 20, known, 0, herbEffects, value, none, noKey);
    CHECK(o.locked);
    CHECK(o.required == 100);

    // Nothing known for it: ERR_USE_LOCKED_WITH_SPELL_S.
    const std::vector<uint32_t> mage = {133};
    o = evaluate(herb(1), 1, 0, 10, mage, 0, herbEffects, value, none, noKey);
    CHECK(o.locked);
    CHECK(o.spell == 0);
    CHECK(lockedRefusal(herb(1), 3, 1, 0, false) == err::kLockedWithSpell);
}

TEST_CASE("go_use: a carried key opens with its own spell", "[go_use]") {
    LockRow r;
    r.type[0] = 1;
    r.index[0] = 5396;
    const std::vector<uint32_t> known;
    const auto effects = [](uint32_t, uint32_t (&)[3], uint32_t (&)[3]) { return false; };
    const auto value = [](uint32_t, int) { return 0; };
    auto o = evaluate(r, 1, 0, 0, known, 0, effects, value, [](uint32_t i) { return i == 5396; },
                      [](uint32_t) { return 6477u; });
    CHECK_FALSE(o.locked);
    CHECK(o.keyItem == 5396);
    CHECK(o.spell == 6477);
    o = evaluate(r, 1, 0, 0, known, 0, effects, value, [](uint32_t) { return false; },
                 [](uint32_t) { return 6477u; });
    CHECK(o.locked);
    CHECK(lockedRefusal(r, 3, 1, 0, false) == err::kLockedWithItem);
}

TEST_CASE("go_use: no lock slot wanting anything is no lock", "[go_use]") {
    const LockRow empty;
    const std::vector<uint32_t> known;
    const auto o = evaluate(empty, 1, 0, 0, known, 0, herbEffects, [](uint32_t, int) { return 0; },
                            [](uint32_t) { return false; }, [](uint32_t) { return 0u; });
    CHECK_FALSE(o.locked);
    CHECK(o.spell == 0);
    // A first slot whose action does not fit: a chest in state 0 is in use.
    CHECK(lockedRefusal(herb(1), 3, 0, 0, false) == err::kChestInUse);
    CHECK(lockedRefusal(herb(1), 10, 0, 0, false) == 0);
}

TEST_CASE("go_use: an effect's value from the caster's skill", "[go_use]") {
    // Herb Gathering: base points -1, 5 a level, the skill over five.
    CHECK(effectValue(-1, 5.0f, 300, 0, 0) == 300.0f);
    CHECK(effectValue(-1, 5.0f, 72, 0, 0) == 70.0f);
    // Pick Lock: base 4, BaseLevel 1.
    CHECK(effectValue(4, 5.0f, 100, 1, 0) == 100.0f);
    // MaxLevel caps the skill at five times it.
    CHECK(effectValue(-1, 5.0f, 300, 0, 20) == 100.0f);
}
