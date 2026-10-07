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
