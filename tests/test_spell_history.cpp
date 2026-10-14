#include <catch_amalgamated.hpp>

#include "game/spell_history.hpp"

// The player's spell history as 0x00807980 reads it: a spell's own cooldown,
// its category's and its global cooldown, the longest of them.

using namespace wowee::game::spell_history;

namespace {
const auto kNoShared = [](uint32_t) { return false; };

/// 0x00805d70's entry for a cast sent at `now`: the global cooldown alone.
Entry globalEntry(uint32_t spell, uint32_t category, int32_t gcd, int64_t now) {
    return Entry{.spellId = spell, .start = now, .categoryStart = now, .gcdCategory = category, .gcd = gcd};
}
}  // namespace

TEST_CASE("the global cooldown holds every spell of its StartRecoveryCategory (0x00807980)", "[spell_history]") {
    History h;
    h.add(globalEntry(133, 133, 1500, 10000));  // Frostbolt sent
    const auto other = h.query(116, 0, 133, 10400, kNoShared);
    REQUIRE(other.running);
    REQUIRE(other.start == 10000);
    REQUIRE(other.duration == 1500);
    REQUIRE(other.enabled);
    // A spell of no StartRecoveryCategory is not held by it.
    REQUIRE_FALSE(h.query(2139, 0, 0, 10400, kNoShared).running);
    // Over, nothing is said: no start, no length.
    const auto after = h.query(116, 0, 133, 11600, kNoShared);
    REQUIRE_FALSE(after.running);
    REQUIRE(after.start == 0);
    REQUIRE(after.duration == 0);
}

TEST_CASE("a spell's own cooldown outlasting the global one is the answer", "[spell_history]") {
    History h;
    h.add(globalEntry(122, 133, 1500, 5000));
    h.add(Entry{.spellId = 122, .start = 5000, .duration = 25000, .categoryStart = 5000});  // Frost Nova's go
    const auto cd = h.query(122, 0, 133, 5500, kNoShared);
    REQUIRE(cd.running);
    REQUIRE(cd.start == 5000);
    REQUIRE(cd.duration == 25000);
    // Another spell waits only for the global cooldown.
    REQUIRE(h.query(116, 0, 133, 5500, kNoShared).duration == 1500);
}

TEST_CASE("a category's cooldown holds the other spells of the category", "[spell_history]") {
    History h;
    // Shocks share category 19 and six seconds.
    h.add(Entry{.spellId = 8042, .start = 0, .category = 19, .categoryStart = 0, .categoryDuration = 6000});
    REQUIRE(h.query(8050, 19, 133, 1000, kNoShared).duration == 6000);
    REQUIRE_FALSE(h.query(8050, 0, 133, 1000, kNoShared).running);
    // A category flagged 0x2 (SpellCategory 351) holds every spell.
    History any;
    any.add(Entry{.spellId = 1, .category = 351, .categoryDuration = 3000});
    REQUIRE(any.query(2, 7, 0, 1000, [](uint32_t c) { return c == 351; }).running);
}

TEST_CASE("a cooldown held until an event counts from now and leaves the spell disabled", "[spell_history]") {
    History h;
    h.add(Entry{.spellId = 5384, .start = 0, .duration = 30000, .categoryStart = 0, .onHold = true});
    const auto cd = h.query(5384, 0, 0, 90000, kNoShared);
    REQUIRE(cd.running);
    REQUIRE(cd.start == 90000);
    REQUIRE(cd.duration == 1);
    REQUIRE_FALSE(cd.enabled);
    // SMSG_COOLDOWN_EVENT starts it (0x00802970).
    h.event(5384, 90000, false);
    const auto running = h.query(5384, 0, 0, 91000, kNoShared);
    REQUIRE(running.start == 90000);
    REQUIRE(running.duration == 30000);
    REQUIRE(running.enabled);
}

TEST_CASE("SMSG_CAST_FAILED takes the cast's global cooldown off (0x00804720)", "[spell_history]") {
    History h;
    h.add(globalEntry(116, 133, 1500, 0));
    h.dropGlobalCooldown(116, 100);
    REQUIRE_FALSE(h.query(133, 0, 133, 200, kNoShared).running);
    REQUIRE(h.entries().empty());
}

TEST_CASE("an item's cooldown is its use spell's as cast from it (0x00807980)", "[spell_history]") {
    const auto noRunes = [](uint32_t) { return false; };
    History h;
    // A Healthstone (item 5512, spell 6262) used: SMSG_SPELL_GO from the
    // item puts the template's two minutes under the item (0x00707e90);
    // SMSG_ITEM_COOLDOWN thirty seconds under another (0x00807060).
    h.add(Entry{.spellId = 6262, .itemId = 5512, .start = 0, .duration = 120000, .category = 4,
                .categoryStart = 0, .categoryDuration = 120000});
    h.add(Entry{.spellId = 6263, .itemId = 19004, .start = 0, .duration = 30000});
    // Asked of the item, the spell's own and its category's run.
    const auto item = h.query(6262, 4, 0, 1000, kNoShared, std::nullopt, noRunes, 5512);
    REQUIRE(item.running);
    REQUIRE(item.duration == 120000);
    // Of the spell with no item (or another item), only the category holds it.
    const auto spell = h.query(6262, 0, 0, 1000, kNoShared, std::nullopt, noRunes, 0);
    REQUIRE_FALSE(spell.running);
    REQUIRE(h.query(6263, 0, 0, 1000, kNoShared, std::nullopt, noRunes, 19004).duration == 30000);
    REQUIRE_FALSE(h.query(6263, 0, 0, 1000, kNoShared, std::nullopt, noRunes, 19005).running);
    // Another item whose template gives the spell category 4 shares it.
    REQUIRE(h.query(6262, 4, 0, 1000, kNoShared, std::nullopt, noRunes, 5511).running);
}
