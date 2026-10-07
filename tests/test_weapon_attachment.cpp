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

TEST_CASE("a unit's sheath state puts its weapons away or draws them (0x0072dbc0)", "[attachment]") {
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2, .subClass = 7};
    const UnitWeaponItem bow{.sheath = 1, .inventoryType = 15, .itemClass = 2, .subClass = 2};
    // Melee: held.
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, sword, SheathState::Melee, false, true, sword) ==
          at::kHandRight);
    // Unarmed and ranged: put away at the Sheath's point.
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, sword, SheathState::Unarmed, false, true, sword) ==
          at::kHipWeaponLeft);
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, sword, SheathState::Ranged, false, true, sword) ==
          at::kHipWeaponRight);
    // The ranged weapon only in the ranged state, or just put away.
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, bow, SheathState::Ranged, false, true, sword) == at::kHandLeft);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, bow, SheathState::Melee, false, true, sword) == at::kNone);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, bow, SheathState::Melee, true, true, sword) ==
          at::kSheathOffHand);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, bow, SheathState::Unarmed, true, true, sword) == at::kNone);
}

TEST_CASE("a creature with a two-hander shows no off hand", "[attachment]") {
    const UnitWeaponItem staff{.sheath = 2, .inventoryType = 17, .itemClass = 2, .subClass = 10};
    const UnitWeaponItem dagger{.sheath = 3, .inventoryType = 13, .itemClass = 2, .subClass = 15};
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, dagger, SheathState::Melee, false, false, staff) == at::kNone);
    // A player keeps it (titan's grip).
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, dagger, SheathState::Melee, false, true, staff) ==
          at::kHandLeft);
    CHECK(unitWeaponPoint(WeaponSlot::OffHand, dagger, SheathState::Melee, false, false, dagger) ==
          at::kHandLeft);
}
