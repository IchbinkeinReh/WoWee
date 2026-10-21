// An item's requirements as the tooltip draws them red (0x006277f0) and as
// "can be used" answers (0x006dc3f0).
#include <catch_amalgamated.hpp>
#include "game/item_requirements.hpp"

using namespace wowee::game;

namespace {
constexpr uint8_t kHuman = 1, kOrc = 2, kBloodElf = 10;
constexpr uint8_t kWarrior = 1, kPaladin = 2, kMage = 8;
constexpr uint32_t kAllMask = 0xFFFFFFFFu;

// A level 80 blood elf mage: cloth only (armour mask bit 1), and staves,
// one-handed swords, daggers and wands among weapons.
ItemRequirementPlayer mage() {
    ItemRequirementPlayer p;
    p.level = 80;
    p.race = kBloodElf;
    p.playerClass = kMage;
    return p;
}
constexpr uint32_t kMageArmour = 1u << 1;
constexpr uint32_t kMageWeapons = (1u << 7) | (1u << 10) | (1u << 15) | (1u << 19);

ItemRequirementFacts plateChest() {
    ItemRequirementFacts f;
    f.itemClass = 4;
    f.subClass = 4;  // plate
    f.inventoryType = 5;
    f.allowableClass = kAllMask;
    f.allowableRace = kAllMask;
    f.requiredLevel = 40;
    return f;
}

std::vector<ItemRestrictionName> races() {
    return {{1, "Human", true},  {2, "Orc", true},     {3, "Dwarf", true},    {4, "Night Elf", true},
            {5, "Undead", true}, {6, "Tauren", true},  {7, "Gnome", true},    {8, "Troll", true},
            {9, "Goblin", false}, {10, "Blood Elf", true}, {11, "Draenei", true}, {12, "Fel Orc", false}};
}
std::vector<ItemRestrictionName> classes() {
    return {{1, "Warrior"}, {2, "Paladin"}, {3, "Hunter"}, {4, "Rogue"},  {5, "Priest"},
            {6, "Death Knight"}, {7, "Shaman"}, {8, "Mage"}, {9, "Warlock"}, {11, "Druid"}};
}
}  // namespace

TEST_CASE("Plate is red for a mage, and not usable", "[item_requirements]") {
    auto p = mage();
    p.proficiencyMask = kMageArmour;
    const auto v = judgeItemRequirements(plateChest(), p);
    CHECK_FALSE(v.subclassOk);
    CHECK(v.levelOk);
    CHECK_FALSE(v.usable());

    // Cloth is fine.
    auto cloth = plateChest();
    cloth.subClass = 1;
    CHECK(judgeItemRequirements(cloth, p).usable());
}

TEST_CASE("A mace is red for a mage; a staff is not", "[item_requirements]") {
    auto p = mage();
    p.proficiencyMask = kMageWeapons;
    ItemRequirementFacts mace;
    mace.itemClass = 2;
    mace.subClass = 4;  // one-handed mace
    mace.inventoryType = 13;
    CHECK_FALSE(judgeItemRequirements(mace, p).subclassOk);
    ItemRequirementFacts staff = mace;
    staff.subClass = 10;
    staff.inventoryType = 17;
    CHECK(judgeItemRequirements(staff, p).subclassOk);
}

TEST_CASE("No proficiency sent for the class: the type is not judged", "[item_requirements]") {
    auto p = mage();
    p.proficiencyMask = 0;
    CHECK(judgeItemRequirements(plateChest(), p).subclassOk);
}

TEST_CASE("Requires Level is red below the level", "[item_requirements]") {
    auto p = mage();
    p.level = 39;
    CHECK_FALSE(judgeItemRequirements(plateChest(), p).levelOk);
    p.level = 40;
    CHECK(judgeItemRequirements(plateChest(), p).levelOk);
}

TEST_CASE("An off-hand weapon's slot is red without dual wield", "[item_requirements]") {
    auto p = mage();
    ItemRequirementFacts offhand;
    offhand.itemClass = 2;
    offhand.subClass = 7;
    offhand.inventoryType = kInvTypeWeaponOffHand;
    CHECK_FALSE(judgeItemRequirements(offhand, p).offHandOk);
    CHECK(judgeItemRequirements(offhand, p).usable());  // not one of CanUseItem's tests
    p.canDualWield = true;
    CHECK(judgeItemRequirements(offhand, p).offHandOk);
}

TEST_CASE("Skill, spell and reputation requirements", "[item_requirements]") {
    auto p = mage();
    ItemRequirementFacts f;
    f.requiredSkill = 197;  // Tailoring
    f.requiredSkillRank = 300;
    p.requiredSkillValue = -1;
    CHECK_FALSE(judgeItemRequirements(f, p).skillOk);  // not learned
    p.requiredSkillValue = 299;
    CHECK_FALSE(judgeItemRequirements(f, p).skillOk);
    p.requiredSkillValue = 300;
    CHECK(judgeItemRequirements(f, p).skillOk);
    // A prospectable or millable item names the skill without requiring it.
    f.flags = kItemFlagProspectable;
    p.requiredSkillValue = -1;
    CHECK(judgeItemRequirements(f, p).skillOk);

    ItemRequirementFacts spell;
    spell.requiredSpell = 26797;
    CHECK_FALSE(judgeItemRequirements(spell, p).spellOk);
    p.knowsRequiredSpell = true;
    CHECK(judgeItemRequirements(spell, p).spellOk);

    ItemRequirementFacts rep;
    rep.requiredReputationFaction = 1090;  // Kirin Tor
    rep.requiredReputationRank = 6;        // revered
    p.requiredFactionValue = 20999;        // honored
    CHECK_FALSE(judgeItemRequirements(rep, p).reputationOk);
    CHECK_FALSE(judgeItemRequirements(rep, p).usable());
    p.requiredFactionValue = 21000;
    CHECK(judgeItemRequirements(rep, p).reputationOk);
}

TEST_CASE("Class and race masks", "[item_requirements]") {
    auto p = mage();
    ItemRequirementFacts f;
    f.allowableClass = (1u << (kWarrior - 1)) | (1u << (kPaladin - 1));
    CHECK_FALSE(judgeItemRequirements(f, p).classOk);
    f.allowableClass = kAllMask;
    f.allowableRace = 1u << (kHuman - 1);
    CHECK_FALSE(judgeItemRequirements(f, p).raceOk);
    f.allowableRace = 0;  // nothing known: not refused
    CHECK(judgeItemRequirements(f, p).raceOk);
}

TEST_CASE("Restriction lines: classes listed, red when not among them", "[item_requirements]") {
    const uint32_t warriorPaladin = (1u << (kWarrior - 1)) | (1u << (kPaladin - 1));
    const auto lines = itemRestrictionLines(warriorPaladin, kAllMask, kBloodElf, kMage, races(), classes());
    CHECK_FALSE(lines.hasOnly);
    CHECK(lines.races.empty());  // every playable race: no line
    REQUIRE(lines.classes.size() == 2);
    CHECK(lines.classes[0] == "Warrior");
    CHECK(lines.classes[1] == "Paladin");
    CHECK_FALSE(lines.classesOk);

    const auto ok = itemRestrictionLines(warriorPaladin, kAllMask, kHuman, kPaladin, races(), classes());
    CHECK(ok.classesOk);
}

TEST_CASE("Restriction lines: the unplayable races do not count", "[item_requirements]") {
    // Every playable race but no goblin or fel orc bit: no races line.
    uint32_t playable = 0;
    for (uint32_t id : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u}) playable |= 1u << (id - 1);
    const auto lines = itemRestrictionLines(kAllMask, playable, kOrc, kWarrior, races(), classes());
    CHECK(lines.races.empty());
    CHECK(lines.classes.empty());
    CHECK_FALSE(lines.hasOnly);
}

TEST_CASE("Restriction lines: one race and one class make an Only line", "[item_requirements]") {
    const auto lines =
        itemRestrictionLines(1u << (kPaladin - 1), 1u << (kHuman - 1), kBloodElf, kMage, races(), classes());
    CHECK(lines.hasOnly);
    CHECK(lines.onlyNames == "HumanPaladin");
    CHECK_FALSE(lines.onlyOk);
    const auto mine = itemRestrictionLines(1u << (kMage - 1), 0, kBloodElf, kMage, races(), classes());
    CHECK(mine.hasOnly);
    CHECK(mine.onlyNames == "Mage");
    CHECK(mine.onlyOk);
}
