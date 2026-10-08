// Where the client hangs a character's weapons and shoulders (0x004eacd0,
// 0x0072b7f0, 0x004ef840).
#include <catch_amalgamated.hpp>

#include "core/weapon_attachment.hpp"

using namespace wowee::core;
namespace at = wowee::core::attachment;

TEST_CASE("drawn weapons are in the hands, a shield on the forearm", "[attachment]") {
    CHECK(weaponAttachmentPoint(WeaponSlot::MainHand, 3, false, false, false) == at::kHandRight);
    CHECK(weaponAttachmentPoint(WeaponSlot::OffHand, 3, false, false, false) == at::kHandLeft);
    CHECK(weaponAttachmentPoint(WeaponSlot::OffHand, 4, false, true, false) == at::kShield);
    // A bow in the left hand, a gun or wand in the right.
    CHECK(weaponAttachmentPoint(WeaponSlot::Ranged, 1, false, false, false) == at::kHandLeft);
    CHECK(weaponAttachmentPoint(WeaponSlot::Ranged, 1, false, false, true) == at::kHandRight);
}

TEST_CASE("put away, a weapon hangs where its Sheath says", "[attachment]") {
    CHECK(weaponAttachmentPoint(WeaponSlot::MainHand, 1, true, false, false) == at::kSheathMainHand);
    CHECK(weaponAttachmentPoint(WeaponSlot::MainHand, 2, true, false, false) == at::kLargeWeaponLeft);
    CHECK(weaponAttachmentPoint(WeaponSlot::MainHand, 3, true, false, false) == at::kHipWeaponLeft);
    CHECK(weaponAttachmentPoint(WeaponSlot::OffHand, 1, true, false, false) == at::kSheathOffHand);
    CHECK(weaponAttachmentPoint(WeaponSlot::OffHand, 2, true, false, false) == at::kLargeWeaponRight);
    CHECK(weaponAttachmentPoint(WeaponSlot::OffHand, 3, true, false, false) == at::kHipWeaponRight);
    CHECK(weaponAttachmentPoint(WeaponSlot::OffHand, 4, true, true, false) == at::kSheathShield);
    CHECK(weaponAttachmentPoint(WeaponSlot::Ranged, 1, true, false, false) == at::kSheathOffHand);
    CHECK(weaponAttachmentPoint(WeaponSlot::Ranged, 1, true, false, true) == at::kSheathMainHand);
    // An off-hand item or a rod has no sheath point: not drawn put away.
    CHECK(weaponAttachmentPoint(WeaponSlot::OffHand, 6, true, false, false) == at::kNone);
    CHECK(weaponAttachmentPoint(WeaponSlot::MainHand, 0, true, false, false) == at::kNone);
}

TEST_CASE("guns, crossbows, wands and thrown weapons are held in the right hand", "[attachment]") {
    CHECK(rangedInRightHand(25));
    CHECK(rangedInRightHand(26));
    CHECK_FALSE(rangedInRightHand(15));
}

TEST_CASE("a shoulder display's first model is the left shoulder", "[attachment]") {
    CHECK(shoulderAttachmentPoint(0) == at::kShoulderLeft);
    CHECK(shoulderAttachmentPoint(1) == at::kShoulderRight);
}

namespace {
UnitWeaponDress dressed(SheathState state, bool isPlayer = true, bool justPutAway = false) {
    return {.state = state, .rangedJustPutAway = justPutAway, .isPlayer = isPlayer};
}
}  // namespace

TEST_CASE("a unit's sheath state puts its weapons away or draws them (0x0072dbc0)", "[attachment]") {
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2, .subClass = 7};
    const UnitWeaponItem bow{.sheath = 1, .inventoryType = 15, .itemClass = 2, .subClass = 2};
    const UnitWeaponItems items{&sword, &sword, &bow};
    // Melee: held.
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, items, dressed(SheathState::Melee)) == at::kHandRight);
    // Unarmed and ranged: put away at the Sheath's point.
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, items, dressed(SheathState::Unarmed)) == at::kHipWeaponLeft);
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, items, dressed(SheathState::Ranged)) == at::kHipWeaponRight);
    // The ranged weapon only in the ranged state, or just put away.
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, items, dressed(SheathState::Ranged)) == at::kHandLeft);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, items, dressed(SheathState::Melee)) == at::kNone);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, items, dressed(SheathState::Melee, true, true)) ==
          at::kSheathOffHand);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, items, dressed(SheathState::Unarmed, true, true)) == at::kNone);
    // An empty slot shows nothing.
    const UnitWeaponItems bare{nullptr, nullptr, nullptr};
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, bare, dressed(SheathState::Melee)) == at::kNone);
}

TEST_CASE("a creature with a two-hander shows no off hand", "[attachment]") {
    const UnitWeaponItem staff{.sheath = 2, .inventoryType = 17, .itemClass = 2, .subClass = 10};
    const UnitWeaponItem dagger{.sheath = 3, .inventoryType = 13, .itemClass = 2, .subClass = 15};
    const UnitWeaponItems twoHander{&staff, &dagger, nullptr};
    const UnitWeaponItems daggers{&dagger, &dagger, nullptr};
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, twoHander, dressed(SheathState::Melee, false)) == at::kNone);
    // A player keeps it (titan's grip).
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, twoHander, dressed(SheathState::Melee, true)) == at::kHandLeft);
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, daggers, dressed(SheathState::Melee, false)) == at::kHandLeft);
}

TEST_CASE("a disarmed unit's weapon is gone (0x00718fc0)", "[attachment]") {
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2, .subClass = 7};
    const UnitWeaponItem shield{.sheath = 4, .inventoryType = 14, .itemClass = 4, .subClass = 6};
    const UnitWeaponItem gun{.sheath = 1, .inventoryType = 26, .itemClass = 2, .subClass = 3};
    const UnitWeaponItem bow{.sheath = 1, .inventoryType = 15, .itemClass = 2, .subClass = 2};
    UnitWeaponDress d = dressed(SheathState::Melee);
    d.unitFlags = kUnitFlagDisarmed;
    // The main hand's weapon goes; a shield stays.
    const UnitWeaponItems swordShield{&sword, &shield, &gun};
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, swordShield, d) == at::kNone);
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, swordShield, d) == at::kShield);
    // With no weapon in the main hand, the off hand's goes instead.
    const UnitWeaponItems offOnly{nullptr, &sword, &bow};
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, offOnly, d) == at::kNone);
    // Drawn ranged: a gun is held in the disarmed main hand, a bow in the
    // off hand - here the one taken.
    d.state = SheathState::Ranged;
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, swordShield, d) == at::kNone);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, offOnly, d) == at::kNone);
    const UnitWeaponItems swordBow{&sword, nullptr, &bow};
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, swordBow, d) == at::kHandLeft);
    // FLAGS_2 0x80 takes the off hand alone.
    UnitWeaponDress off = dressed(SheathState::Melee);
    off.unitFlags2 = kUnitFlag2DisarmOffhand;
    const UnitWeaponItems daggers{&sword, &sword, nullptr};
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, daggers, off) == at::kHandRight);
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, daggers, off) == at::kNone);
}

TEST_CASE("an unarmed animation dresses the off hand by 0x00715d00", "[attachment]") {
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2, .subClass = 7};
    const UnitWeaponItem book{.sheath = 0, .inventoryType = 23, .itemClass = 4, .subClass = 0};
    const UnitWeaponItem shield{.sheath = 4, .inventoryType = 14, .itemClass = 4, .subClass = 6};
    // 0x00721ed0: unarmed attack / parry / ready, or an empty-handed one
    // with nothing in the main hand.
    CHECK(offHandFollowsAnimation(16, true));
    CHECK(offHandFollowsAnimation(25, true));
    CHECK(offHandFollowsAnimation(17, false));
    CHECK_FALSE(offHandFollowsAnimation(17, true));
    CHECK_FALSE(offHandFollowsAnimation(0, false));
    CHECK_FALSE(offHandFollowsAnimation(kNoAnimationBehavior, false));
    // Melee with an empty main hand: a held item is put away.
    UnitWeaponDress d = dressed(SheathState::Melee);
    d.offHandFollowsAnimation = true;
    const UnitWeaponItems bookOnly{nullptr, &book, nullptr};
    CHECK(offHandAnimationSheath(SheathState::Melee, bookOnly) == SheathState::Unarmed);
    // Unarmed with a shield in the off hand: drawn.
    d.state = SheathState::Unarmed;
    const UnitWeaponItems shieldOnly{nullptr, &shield, nullptr};
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, shieldOnly, d) == at::kShield);
    const UnitWeaponItems swordSword{&sword, &sword, nullptr};
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, swordSword, d) == at::kHandLeft);
    // The main hand is not affected.
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, swordSword, d) == at::kHipWeaponLeft);
}

TEST_CASE("the sheath key cycles as 0x006e23a0 does", "[attachment]") {
    CHECK(toggledSheathState(SheathState::Unarmed, true, true) == SheathState::Melee);
    CHECK(toggledSheathState(SheathState::Unarmed, false, true) == SheathState::Ranged);
    CHECK(toggledSheathState(SheathState::Unarmed, false, false) == SheathState::Unarmed);
    CHECK(toggledSheathState(SheathState::Melee, true, true) == SheathState::Ranged);
    CHECK(toggledSheathState(SheathState::Melee, true, false) == SheathState::Unarmed);
    CHECK(toggledSheathState(SheathState::Ranged, true, true) == SheathState::Unarmed);
}

#include "core/preview_dressing.hpp"

TEST_CASE("the select screen wears the head and draws the weapons (0x004e3cd0)", "[attachment]") {
    std::vector<PreviewItem> slots(23);
    slots[0] = {.display = 100, .inventoryType = 1};
    slots[4] = {.display = 104, .inventoryType = 5};
    slots[14] = {.display = 114, .inventoryType = 16};
    slots[15] = {.display = 115, .inventoryType = 13};
    slots[16] = {.display = 116, .inventoryType = 14};
    slots[17] = {.display = 117, .inventoryType = 15};
    slots[19] = {.display = 119, .inventoryType = 18};
    slots[21] = {.display = 121, .inventoryType = 18};
    const PreviewDress warrior = characterSelectDress(slots, 1, 0);
    CHECK(warrior.head == 100);
    REQUIRE(warrior.worn.size() == 2);  // chest and cloak
    REQUIRE(warrior.held.size() == 2);
    CHECK(warrior.held[0].point == at::kHandRight);
    CHECK(warrior.held[1].point == at::kShield);
    CHECK(warrior.held[1].shield);
    // The bags go to the quiver slot, in order.
    CHECK(warrior.quivers == std::vector<uint32_t>{119, 121});
    // A hidden helm and cloak stay off; a hunter holds the bow alone, left hand.
    const PreviewDress hidden =
        characterSelectDress(slots, 1, kCharacterFlagHideCloak | kCharacterFlagHideHelm);
    CHECK(hidden.worn.size() == 1);
    CHECK(hidden.head == 0);
    const PreviewDress hunter = characterSelectDress(slots, kClassHunter, 0);
    REQUIRE(hunter.held.size() == 1);
    CHECK(hunter.held[0].display == 117);
    CHECK(hunter.held[0].point == at::kHandLeft);
}

TEST_CASE("the create screen wears the start outfit, no head", "[attachment]") {
    const std::vector<PreviewItem> outfit = {
        {.display = 1, .inventoryType = 1},  {.display = 5, .inventoryType = 5},
        {.display = 17, .inventoryType = 17}, {.display = 26, .inventoryType = 26},
        {.display = 22, .inventoryType = 22}};
    const PreviewDress dk = characterCreateDress(outfit, 6);
    REQUIRE(dk.worn.size() == 1);
    REQUIRE(dk.held.size() == 2);
    CHECK(dk.held[0].point == at::kHandRight);
    CHECK(dk.held[1].point == at::kHandLeft);
    const PreviewDress hunter = characterCreateDress(outfit, kClassHunter);
    REQUIRE(hunter.held.size() == 1);
    CHECK(hunter.held[0].point == at::kHandRight);  // a gun, in the right hand
    CHECK(hunter.quivers.empty());
}

TEST_CASE("the create screen's hunter wears its quiver (0x004e0fd0 case 0x12)", "[attachment]") {
    const std::vector<PreviewItem> outfit = {{.display = 15, .inventoryType = 15},
                                             {.display = 18, .inventoryType = 18}};
    CHECK(characterCreateDress(outfit, kClassHunter).quivers == std::vector<uint32_t>{18});
    // Only a hunter's.
    CHECK(characterCreateDress(outfit, 1).quivers.empty());
}
