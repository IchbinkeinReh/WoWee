// The client's name and nameplate questions (0x00729c70, 0x0072b060) and the
// cvars behind them (0x007e6150, 0x00511xxx).

#include <catch_amalgamated.hpp>

#include "game/unit_name_rules.hpp"

#include <map>
#include <string>

namespace un = wowee::game::unit_names;

namespace {
uint32_t defaultMask() {
    return un::nameMask([](const char*, const char* def) { return std::string(def); });
}
un::UnitFacts npc() { return {}; }
un::UnitFacts player(bool friendly) {
    un::UnitFacts f;
    f.isPlayer = true;
    f.friendlyGroup = friendly;
    f.attackable = !friendly;
    return f;
}
}  // namespace

TEST_CASE("name cvars default as 0x007e6150 registers them", "[unit_names]") {
    std::map<std::string, std::string> d;
    for (const auto& c : un::kNameCVars) d[c.name] = c.defaultValue;
    CHECK(d.size() == 13);
    CHECK(d["UnitNameOwn"] == "0");
    CHECK(d["UnitNameNPC"] == "0");
    CHECK(d["UnitNameNonCombatCreatureName"] == "0");
    CHECK(d["UnitNameEnemyGuardianName"] == "0");
    CHECK(d["UnitNameFriendlyGuardianName"] == "0");
    CHECK(d["UnitNameEnemyTotemName"] == "0");
    CHECK(d["UnitNameFriendlyTotemName"] == "0");
    CHECK(d["UnitNamePlayerGuild"] == "1");
    CHECK(d["UnitNamePlayerPVPTitle"] == "1");
    CHECK(d["UnitNameEnemyPlayerName"] == "1");
    CHECK(d["UnitNameFriendlyPlayerName"] == "1");
    CHECK(d["UnitNameEnemyPetName"] == "1");
    CHECK(d["UnitNameFriendlyPetName"] == "1");
    CHECK(defaultMask() == (un::kPlayerGuild | un::kPlayerPvpTitle | un::kEnemyPlayer | un::kEnemyPet |
                            un::kFriendlyPlayer | un::kFriendlyPet));
}

TEST_CASE("nameplate cvars default off for both sides, on for every kind", "[unit_names]") {
    std::map<std::string, std::string> d;
    for (const auto& c : un::kPlateCVars) d[c.name] = c.defaultValue;
    CHECK(d["nameplateShowEnemies"] == "0");
    CHECK(d["nameplateShowFriends"] == "0");
    CHECK(d["nameplateShowFriendlyTotems"] == "1");
    CHECK(d["nameplateShowEnemyTotems"] == "1");
    CHECK(d["nameplateShowFriendlyPets"] == "1");
    CHECK(d["nameplateShowEnemyGuardians"] == "1");
    CHECK(d["nameplateAllowOverlap"] == "1");
}

TEST_CASE("who is named over their head by default", "[unit_names]") {
    const uint32_t mask = defaultMask();
    // Creatures are not, players on both sides are.
    CHECK_FALSE(un::nameShown(npc(), mask, false, false));
    CHECK(un::nameShown(player(true), mask, false, false));
    CHECK(un::nameShown(player(false), mask, false, false));
    // The target always is - unless it has a plate, which names it.
    CHECK(un::nameShown(npc(), mask, true, false));
    CHECK_FALSE(un::nameShown(npc(), mask, true, true));
    CHECK_FALSE(un::nameShown(player(true), mask, false, true));
    // Nothing unselectable.
    un::UnitFacts trigger = npc();
    trigger.notSelectable = true;
    CHECK_FALSE(un::nameShown(trigger, mask, true, false));
    // UnitNameNPC on names creatures but not critters.
    CHECK(un::nameShown(npc(), mask | un::kNpc, false, false));
    un::UnitFacts critter = npc();
    critter.creatureType = un::kCreatureTypeCritter;
    CHECK_FALSE(un::nameShown(critter, mask | un::kNpc, false, false));
    CHECK(un::nameShown(critter, mask | un::kNonCombatCreature, false, false));
    // The player answers to UnitNameOwn alone.
    CHECK_FALSE(un::ownNameShown(mask));
    CHECK(un::ownNameShown(mask | un::kOwn));
}

TEST_CASE("stealth hides enemies' names, and a player's pets and totems answer their rows",
          "[unit_names]") {
    const uint32_t mask = defaultMask() | un::kNpc;
    un::UnitFacts sneak = player(false);
    sneak.creeping = true;
    CHECK_FALSE(un::nameShown(sneak, mask, false, false));
    un::UnitFacts mob = npc();
    mob.attackable = true;
    mob.creeping = true;
    CHECK_FALSE(un::nameShown(mob, mask, false, false));

    un::UnitFacts totem = npc();
    totem.ownerIsPlayer = true;
    totem.createdBy = totem.summonedBy = true;
    totem.creatureType = un::kCreatureTypeTotem;
    CHECK_FALSE(un::nameShown(totem, mask, false, false));
    CHECK(un::nameShown(totem, mask | un::kFriendlyTotem, false, false));
    totem.attackable = true;
    CHECK(un::nameShown(totem, mask | un::kEnemyTotem, false, false));

    un::UnitFacts pet = npc();
    pet.ownerIsPlayer = true;
    pet.createdBy = pet.summonedBy = true;
    pet.friendlyGroup = true;
    CHECK(un::nameShown(pet, mask, false, false));
    CHECK_FALSE(un::nameShown(pet, mask & ~un::kFriendlyPet, false, false));

    // Made but neither charmed nor summoned: a guardian, on the enemy row
    // whichever side it is.
    un::UnitFacts guardian = npc();
    guardian.ownerIsPlayer = true;
    guardian.createdBy = true;
    guardian.friendlyGroup = true;
    CHECK_FALSE(un::nameShown(guardian, mask, false, false));
    CHECK(un::nameShown(guardian, mask | un::kEnemyGuardian, false, false));
    CHECK_FALSE(un::nameShown(guardian, mask | un::kFriendlyGuardian, false, false));
}

TEST_CASE("who gets a nameplate", "[unit_names]") {
    un::PlateSwitches off;
    un::UnitFacts mob = npc();
    mob.attackable = true;
    CHECK_FALSE(un::plateShown(mob, off));
    un::PlateSwitches enemies;
    enemies.enemies = true;
    CHECK(un::plateShown(mob, enemies));
    CHECK_FALSE(un::plateShown(npc(), enemies));  // a friendly creature
    un::PlateSwitches friends;
    friends.friends = true;
    CHECK(un::plateShown(npc(), friends));
    CHECK(un::plateShown(player(true), friends));
    // None for the dead, critters, or an enemy player out of reach of attack.
    un::UnitFacts dead = mob;
    dead.alive = false;
    CHECK_FALSE(un::plateShown(dead, enemies));
    un::UnitFacts critter = mob;
    critter.creatureType = un::kCreatureTypeCritter;
    CHECK_FALSE(un::plateShown(critter, enemies));
    un::UnitFacts sanctuary = player(false);
    sanctuary.attackable = false;
    CHECK_FALSE(un::plateShown(sanctuary, enemies));
    // A player's totem answers the totem switch.
    un::UnitFacts totem = mob;
    totem.ownerIsPlayer = true;
    totem.createdBy = totem.summonedBy = true;
    totem.creatureType = un::kCreatureTypeTotem;
    CHECK(un::plateShown(totem, enemies));
    enemies.enemyTotems = false;
    CHECK_FALSE(un::plateShown(totem, enemies));
}

TEST_CASE("name text height and the raid icon's fade", "[unit_names]") {
    CHECK(un::textHeight(2.0f) == Catch::Approx(0.2f));
    CHECK(un::textHeight(4.0f) == Catch::Approx(0.2f));
    CHECK(un::textHeight(8.0f) == Catch::Approx(8.0f * 0.375f * 0.2f));
    CHECK(un::raidIconAlpha(2.0f) == 0x22);
    CHECK(un::raidIconAlpha(9.0f) == 0xff);
    CHECK(un::raidIconAlpha(5.0f) == 89);
    CHECK(un::playerNamePrefix(0x2 | 0x4, "<AFK>", "<DND>", "<GM>") == "<AFK><DND>");
    CHECK(un::playerNamePrefix(0x8, "<AFK>", "<DND>", "<GM>") == "<GM>");
    CHECK(un::playerNamePrefix(0x8 | 0x8000, "<AFK>", "<DND>", "<GM>") == "<Dev>");
}

TEST_CASE("the line under a made unit's name (0x0061e830)", "[unit_names]") {
    // A summon's SummonProperties Title, 0 for none; -1 or none falls back to
    // the creature type: a beast's Pet, anything else's Minion.
    CHECK(un::summonTitle(std::nullopt, un::kCreatureTypeBeast) == 1);
    CHECK(un::summonTitle(std::nullopt, 3) == 3);
    CHECK(un::summonTitle(-1, un::kCreatureTypeBeast) == 1);
    CHECK(un::summonTitle(2, 3) == 2);
    CHECK(un::summonTitle(0, 3) == 0);
    CHECK(un::summonTitleText(1, "Thrall") == "Thrall's Pet");
    CHECK(un::summonTitleText(4, "Thrall") == "Thrall's Totem");
    CHECK(un::summonTitleText(0, "Thrall").empty());
    CHECK(un::summonTitleText(99, "Thrall").empty());
}

TEST_CASE("the plate's threat flash and name colours", "[unit_names][nameplate]") {
    CHECK(un::threatWarningOn(3, false, false));
    CHECK_FALSE(un::threatWarningOn(0, true, true));
    CHECK(un::threatWarningOn(1, true, false));
    CHECK_FALSE(un::threatWarningOn(1, false, true));
    CHECK(un::threatWarningOn(2, false, true));
    CHECK(un::plateThreatColor(0) == 0u);
    CHECK(un::plateThreatColor(1) == 0xffffff77u);
    CHECK(un::plateThreatColor(2) == 0xffff9900u);
    CHECK(un::plateThreatColor(3) == 0xffff0000u);
    CHECK(un::plateNameColor(true, true) == 0xffff0000u);
    CHECK(un::plateNameColor(false, true) == 0xffffff00u);
    CHECK(un::plateNameColor(false, false) == 0xffffffffu);
}
