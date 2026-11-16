// The floaters over the world and the settings that gate them
// (0x0071f990, 0x00722340, 0x00722440, 0x00719220, 0x0074da00).
#include <catch_amalgamated.hpp>
#include "game/combat_text_filter.hpp"

#include <string>

using namespace wowee::game;
using T = CombatTextEntry;

namespace {
bool shown(T::Type type, FloaterSource src, bool meleeLike = true, const FloaterSettings& s = {},
           bool onPlayer = false) {
    return worldFloaterShown(type, src, onPlayer, false, meleeLike, s);
}
}  // namespace

TEST_CASE("the player's and its units' hits are floaters, no one else's", "[combattext]") {
    CHECK(shown(T::MELEE_DAMAGE, FloaterSource::Player));
    CHECK(shown(T::SPELL_DAMAGE, FloaterSource::PlayersUnit, false));
    CHECK_FALSE(shown(T::MELEE_DAMAGE, FloaterSource::Other));
}

TEST_CASE("nothing floats over the active player", "[combattext]") {
    CHECK_FALSE(shown(T::MELEE_DAMAGE, FloaterSource::Player, true, {}, true));
    CHECK_FALSE(shown(T::HEAL, FloaterSource::Player, true, {}, true));
}

TEST_CASE("CombatDamage gates damage and misses for everyone", "[combattext]") {
    FloaterSettings off;
    off.combatDamage = false;
    CHECK_FALSE(shown(T::MELEE_DAMAGE, FloaterSource::Player, true, off));
    CHECK_FALSE(shown(T::DODGE, FloaterSource::Player, true, off));
    CHECK_FALSE(shown(T::MELEE_DAMAGE, FloaterSource::PlayersUnit, true, off));
    CHECK(shown(T::HEAL, FloaterSource::Player, true, off));
}

TEST_CASE("a periodic tick also needs CombatLogPeriodicSpells", "[combattext]") {
    FloaterSettings off;
    off.periodicSpells = false;
    CHECK_FALSE(shown(T::PERIODIC_DAMAGE, FloaterSource::Player, false, off));
    CHECK(shown(T::SPELL_DAMAGE, FloaterSource::Player, false, off));
}

TEST_CASE("a pet's swing and spell answer to their own rows", "[combattext]") {
    FloaterSettings melee;
    melee.petMeleeDamage = false;
    CHECK_FALSE(shown(T::MELEE_DAMAGE, FloaterSource::PlayersUnit, true, melee));
    CHECK(shown(T::SPELL_DAMAGE, FloaterSource::PlayersUnit, false, melee));
    // The player's own swing is not the pet's row.
    CHECK(shown(T::MELEE_DAMAGE, FloaterSource::Player, true, melee));
    FloaterSettings spell;
    spell.petSpellDamage = false;
    CHECK_FALSE(shown(T::SPELL_DAMAGE, FloaterSource::PlayersUnit, false, spell));
    CHECK(shown(T::MISS, FloaterSource::PlayersUnit, true, spell));
}

TEST_CASE("healing floats for the player's own heals alone", "[combattext]") {
    CHECK(shown(T::HEAL, FloaterSource::Player));
    CHECK_FALSE(shown(T::HEAL, FloaterSource::PlayersUnit));
    FloaterSettings off;
    off.combatHealing = false;
    CHECK_FALSE(shown(T::CRIT_HEAL, FloaterSource::Player, true, off));
}

TEST_CASE("lines that are not floaters are never drawn over the world", "[combattext]") {
    CHECK_FALSE(shown(T::XP_GAIN, FloaterSource::Player));
    CHECK_FALSE(shown(T::HONOR_GAIN, FloaterSource::Player));
    CHECK_FALSE(shown(T::ENERGIZE, FloaterSource::Player));
    CHECK_FALSE(shown(T::INTERRUPT, FloaterSource::Player));
}

TEST_CASE("misses show the client's global strings", "[combattext]") {
    CHECK(std::string(missFloaterKey(T::MISS)) == "COMBAT_TEXT_MISS");
    CHECK(std::string(missFloaterKey(T::REFLECT)) == "COMBAT_TEXT_REFLECT");
    CHECK(missFloaterKey(T::MELEE_DAMAGE) == nullptr);
}

TEST_CASE("threat status as the client keeps it", "[combattext]") {
    const uint32_t clear[] = {1000, 500};
    CHECK(clientThreatStatus(clear, 2, 0) == 4);
    CHECK(clientThreatStatus(clear, 2, 1) == 1);
    CHECK(clientThreatStatus(clear, 2, 2) == 0);
    const uint32_t tied[] = {1000, 1000};
    CHECK(clientThreatStatus(tied, 2, 0) == 3);
    CHECK(clientThreatStatus(tied, 2, 1) == 2);
}

TEST_CASE("the threat floater skips 2 to 1 and 3 to 4", "[combattext]") {
    CHECK_FALSE(threatFloaterShown(2, 1));
    CHECK_FALSE(threatFloaterShown(3, 4));
    CHECK(threatFloaterShown(1, 4));
    CHECK(threatFloaterShown(4, 1));
}

TEST_CASE("mechanic floaters follow the fct settings", "[combattext]") {
    CHECK(mechanicFloaterShown(true, false, false, false));
    CHECK(mechanicFloaterShown(false, true, false, true));
    CHECK_FALSE(mechanicFloaterShown(false, true, false, false));
    CHECK(mechanicFloaterShown(false, true, true, false));
    CHECK_FALSE(mechanicFloaterShown(false, false, true, true));
}
