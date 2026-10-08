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

TEST_CASE("a pre-WotLK creature's slot reads from its INFO pair", "[attachment]") {
    // Class 2, subclass 7, material 1, inventory type 13; Sheath 3.
    const UnitWeaponItem sword = virtualItemInfo(0x0D010702u, 0x00000003u);
    CHECK(sword.itemClass == 2);
    CHECK(sword.subClass == 7);
    CHECK(sword.inventoryType == 13);
    CHECK(sword.sheath == 3);
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

TEST_CASE("the animation changes the sheath state (0x00738180)", "[attachment]") {
    using S = SheathState;
    AnimationSheathInput in{.current = S::Melee, .animId = 0, .animKnown = true, .activePlayer = true};
    // WeaponFlags 4 puts the weapons away, in any state.
    in.weaponFlags = 4;
    CHECK(animationSheathState(in) == S::Unarmed);
    in.keepWeapons = true;
    CHECK(animationSheathState(in) == std::nullopt);
    in.keepWeapons = false;
    // 0x10 away, 0x20 melee.
    in.current = S::Unarmed;
    in.weaponFlags = 0x20;
    CHECK(animationSheathState(in) == S::Melee);
    in.current = S::Melee;
    in.weaponFlags = 0x10;
    CHECK(animationSheathState(in) == S::Unarmed);
    // Nothing for the active player otherwise; another unit takes the field.
    in.weaponFlags = 0;
    in.field = S::Unarmed;
    CHECK(animationSheathState(in) == std::nullopt);
    in.activePlayer = false;
    CHECK(animationSheathState(in) == S::Unarmed);
}

TEST_CASE("casting and attacking set the sheath state (0x00738180)", "[attachment]") {
    using S = SheathState;
    AnimationSheathInput in{.current = S::Melee, .animId = 0, .animKnown = true, .weaponFlags = 0x20,
                            .activePlayer = true};
    // A cast puts the weapons away - not one whose spell has 0x40000 - and
    // the animation's flags then count for nothing.
    in.casting = true;
    in.castSheathes = true;
    CHECK(animationSheathState(in) == S::Unarmed);
    in.castSheathes = false;
    CHECK(animationSheathState(in) == std::nullopt);
    in.casting = false;
    // Attacking draws melee.
    in.current = S::Unarmed;
    in.weaponFlags = 0x10;
    in.attacking = true;
    in.behavior = 0x10;  // a combat behavior (0x0071d590)
    CHECK(animationSheathState(in) == S::Melee);
    in.behavior = kNoAnimationBehavior;
    CHECK(animationSheathState(in) == S::Unarmed);
    in.weaponFlags = 0;
    CHECK(animationSheathState(in) == S::Melee);
}

TEST_CASE("the ranged state holds through loading and shooting (0x00738180)", "[attachment]") {
    using S = SheathState;
    AnimationSheathInput in{.current = S::Ranged, .animKnown = true, .weaponFlags = 0x20,
                            .casting = true, .castSheathes = true, .attacking = true, .activePlayer = true};
    for (uint32_t load : {105u, 106u, 112u}) {
        in.animId = load;
        CHECK(animationSheathState(in) == std::nullopt);
    }
    in.animId = 46;
    in.behavior = 0x2e;  // a ranged behavior (0x0071d2e0)
    CHECK(animationSheathState(in) == std::nullopt);
    // Any other animation with the flags moves it; the cast and the attack
    // do not.
    in.behavior = kNoAnimationBehavior;
    CHECK(animationSheathState(in) == S::Melee);
    in.weaponFlags = 0x10;
    CHECK(animationSheathState(in) == S::Unarmed);
    in.weaponFlags = 0;
    CHECK(animationSheathState(in) == std::nullopt);
    in.animKnown = false;
    in.weaponFlags = 0x10;
    CHECK(animationSheathState(in) == std::nullopt);
}

TEST_CASE("the field moves the active player only from the state it left (0x00737aa0)", "[attachment]") {
    using S = SheathState;
    CHECK(fieldSheathChange(S::Melee, S::Melee, S::Unarmed, true) == S::Unarmed);
    CHECK(fieldSheathChange(S::Ranged, S::Melee, S::Unarmed, true) == std::nullopt);
    CHECK(fieldSheathChange(S::Ranged, S::Melee, S::Unarmed, false) == S::Unarmed);
}

TEST_CASE("a spell's cast sets the sheath state (0x007fa2e0, 0x0073a6c0)", "[attachment]") {
    using S = SheathState;
    // Auto Shot: Attributes 2.
    CHECK(spellSheathState({.known = true, .attributes = 2}) == S::Ranged);
    CHECK(spellSheathState({.known = true, .attributes = 0}) == std::nullopt);
    // A weapon thrown as the missile.
    CHECK(spellSheathState({.known = true, .missileModel = -1}) == S::Melee);
    CHECK(spellSheathState({.known = true, .missileModel = -2}) == S::Melee);
    // A kit with a weapon effect, unless the spell has 0x40000 or the model
    // CreatureModelData flag 0x10.
    CHECK(spellSheathState({.known = true, .kitWeaponEffect = true}) == S::Unarmed);
    CHECK(spellSheathState({.known = false, .kitWeaponEffect = true}) == S::Unarmed);
    CHECK(spellSheathState({.known = true, .attributes = 0x40000, .kitWeaponEffect = true}) == std::nullopt);
    CHECK(spellSheathState({.known = true, .kitWeaponEffect = true, .modelHoldsEffects = false}) ==
          std::nullopt);
    CHECK(spellSheathState({.known = true, .attributes = 2, .kitWeaponEffect = true}) == S::Unarmed);
}

TEST_CASE("a state asked for is taken as 0x00736d30 lets it", "[attachment]") {
    using S = SheathState;
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2, .subClass = 7};
    const UnitWeaponItems items{&sword, nullptr, nullptr};
    SheathSetInput in{.current = S::Melee};
    CHECK(sheathStateChange(S::Unarmed, items, in) == S::Unarmed);
    // Unchanged: nothing.
    CHECK(sheathStateChange(S::Melee, items, in) == std::nullopt);
    // Ranged: a player only when its class may; a creature always.
    in.isPlayer = true;
    CHECK(sheathStateChange(S::Ranged, items, in) == std::nullopt);
    in.classMayDrawRanged = true;
    CHECK(sheathStateChange(S::Ranged, items, in) == S::Ranged);
    in.isPlayer = false;
    in.classMayDrawRanged = false;
    CHECK(sheathStateChange(S::Ranged, items, in) == S::Ranged);
    // CREATURE_TYPEFLAGS 0x10000000: only the server's.
    in.doNotSheathe = true;
    CHECK(sheathStateChange(S::Unarmed, items, in) == std::nullopt);
    in.fromServer = true;
    CHECK(sheathStateChange(S::Unarmed, items, in) == S::Unarmed);
    // No model: nothing.
    in = {.current = S::Melee, .hasModel = false};
    CHECK(sheathStateChange(S::Unarmed, items, in) == std::nullopt);
}

TEST_CASE("an unarmed animation decides the state asked for (0x00736d30 with 0x00715d00)", "[attachment]") {
    using S = SheathState;
    // Melee with nothing in the hands is unarmed: no change from unarmed.
    const UnitWeaponItems empty{nullptr, nullptr, nullptr};
    const SheathSetInput in{.current = S::Unarmed, .offHandFollowsAnimation = true};
    CHECK(sheathStateChange(S::Melee, empty, in) == std::nullopt);
    CHECK(sheathStateChange(S::Melee, empty, SheathSetInput{.current = S::Unarmed}) == S::Melee);
}

TEST_CASE("a stand state but standing or a chair puts the weapons away (0x0073f060)", "[attachment]") {
    using S = SheathState;
    CHECK_FALSE(standStateSheathes(S::Melee, 0));
    CHECK(standStateSheathes(S::Melee, 1));
    CHECK_FALSE(standStateSheathes(S::Melee, 2));
    CHECK(standStateSheathes(S::Ranged, 3));
    CHECK(standStateSheathes(S::Melee, 8));
    CHECK_FALSE(standStateSheathes(S::Unarmed, 1));
}

TEST_CASE("a channel at a fishing bobber draws the pole (0x0073a520)", "[attachment]") {
    using S = SheathState;
    CHECK(channelSheathState(S::Unarmed, kGameObjectTypeFishingNode, 7620) == S::Melee);
    CHECK(channelSheathState(S::Ranged, kGameObjectTypeFishingNode, 7620) == S::Melee);
    CHECK(channelSheathState(S::Melee, kGameObjectTypeFishingNode, 7620) == std::nullopt);
    CHECK(channelSheathState(S::Unarmed, kGameObjectTypeFishingNode, 0) == std::nullopt);
    CHECK(channelSheathState(S::Unarmed, 3, 7620) == std::nullopt);
}

TEST_CASE("a text emote is sent, or refused, as 0x006dd9e0 decides", "[attachment]") {
    using V = TextEmoteVerdict;
    auto v = [](TextEmoteSheathInput in) { return textEmoteVerdict(in); };
    CHECK(v({.emoteKnown = true}) == V::Send);
    // No Emotes row, or possessed: nothing.
    CHECK(v({.emoteKnown = false}) == V::Refused);
    CHECK(v({.emoteKnown = true, .unitFlags = 0x1000000}) == V::Refused);
    // Asleep: only an emote whose EmoteSpecProc is 1, and one allowed asleep.
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x200, .standState = 3}) == V::Refused);
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x200, .specProc = 1, .standState = 3}) == V::Send);
    CHECK(v({.emoteKnown = true, .specProc = 1, .standState = 3}) == V::Refused);
    // 1: only standing; 0x80: not swimming; 0x8000: not flying, nor on a
    // flying spline (0x004f5260).
    CHECK(v({.emoteKnown = true, .emoteFlags = 1, .standState = 1}) == V::Refused);
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x80, .moveFlags = 0x200000}) == V::Refused);
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x8000, .moveFlags = 0x2000000}) == V::Refused);
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x8000, .onFlyingSpline = true}) == V::Refused);
    CHECK(v({.emoteKnown = true, .onFlyingSpline = true}) == V::Send);
    // 0x4000 refused while moving with ERR_NOEMOTEWHILERUNNING, unless the
    // player is not its own master.
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x4000, .moveFlags = 1}) == V::RefusedWhileMoving);
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x4000, .moveFlags = 1, .charmed = true}) == V::Send);
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x4000, .moveFlags = 1, .unitFlags = 0x800000}) ==
          V::Send);
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x4000}) == V::Send);
    // An emote not allowed at all is refused before the moving check.
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x4001, .standState = 1, .moveFlags = 1}) == V::Refused);
    // 0x400: never.
    CHECK(v({.emoteKnown = true, .emoteFlags = 0x400}) == V::Refused);
}

TEST_CASE("another unit takes the field's state whatever it was in (0x00737aa0)", "[attachment]") {
    using S = SheathState;
    CHECK(fieldSheathChange(S::Unarmed, S::Melee, S::Ranged, false) == S::Ranged);
    // 0x00738180 brings it back to the field after its own change.
    AnimationSheathInput in{.current = S::Unarmed, .animId = 0, .animKnown = true,
                            .behavior = 0, .activePlayer = false, .field = S::Melee};
    CHECK(animationSheathState(in) == S::Melee);
}

TEST_CASE("a kit's weapon effects hang in the hands (0x0073a6c0)", "[attachment]") {
    CHECK(kitWeaponEffectAttachment(true) == at::kHandLeft);
    CHECK(kitWeaponEffectAttachment(false) == at::kHandRight);
}

TEST_CASE("an attached effect is sized as 0x006f8c50 sizes it", "[attachment]") {
    // The model's AttachedEffectScale times the effect's Scale.
    CHECK(kitEffectScale(1.5f, 2.0f, 1.0f, 0.0f, 100.0f) == Catch::Approx(3.0f));
    // Held within the allowed scales as the attachment shows it.
    CHECK(kitEffectScale(1.0f, 4.0f, 2.0f, 0.0f, 4.0f) == Catch::Approx(2.0f));
    CHECK(kitEffectScale(1.0f, 0.1f, 1.0f, 0.5f, 4.0f) == Catch::Approx(0.5f));
    CHECK(kitEffectScale(1.0f, 0.1f, 2.0f, 0.5f, 4.0f) == Catch::Approx(0.25f));
    // Nothing to hold when nothing shows.
    CHECK(kitEffectScale(1.0f, 0.0f, 1.0f, 0.5f, 4.0f) == Catch::Approx(0.0f));
}

TEST_CASE("the sheath key's reach draws from unarmed (0x007369b0)", "[attachment][reach]") {
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2};
    const UnitWeaponItem axe{.sheath = 1, .inventoryType = 13, .itemClass = 2};
    const UnitWeaponItems items{&sword, &axe, nullptr};
    ReachPlay play;
    SheathReach reach = beginSheathReach(SheathState::Unarmed, SheathState::Melee, items, play);
    // A hip weapon (Sheath 3) reaches with HipSheath, one on the back with Sheath.
    CHECK(play.play[0]);
    CHECK(play.animId[0] == kAnimHipSheath);
    CHECK(play.play[1]);
    CHECK(play.animId[1] == kAnimSheath);
    CHECK(reach.drawing[0]);
    CHECK(reach.drawing[1]);
    CHECK(reach.shown == weaponsShownFor(SheathState::Unarmed));
    // Each weapon comes to the hand at its own event.
    sheathReachSwap(reach, 0, items);
    CHECK(reach.shown.mainHeld);
    CHECK_FALSE(reach.shown.offHeld);
    sheathReachSwap(reach, 1, items);
    CHECK(reach.shown.offHeld);
    const ReachPlay after = sheathReachEnded(reach, 0, items);
    CHECK_FALSE(after.play[0]);
}

TEST_CASE("the sheath key's reach puts the weapons away and stops (0x00736b60)", "[attachment][reach]") {
    const UnitWeaponItem sword{.sheath = 1, .inventoryType = 17, .itemClass = 2};
    const UnitWeaponItems items{&sword, nullptr, nullptr};
    ReachPlay play;
    SheathReach reach = beginSheathReach(SheathState::Melee, SheathState::Unarmed, items, play);
    CHECK(play.play[0]);
    CHECK_FALSE(play.play[1]);
    CHECK_FALSE(reach.drawing[0]);
    // The empty hand went straight to its second half.
    CHECK(reach.drawing[1]);
    CHECK(reach.shown.mainHeld);
    sheathReachSwap(reach, 0, items);
    CHECK_FALSE(reach.shown.mainHeld);
    // Ended: the second half has nothing to draw.
    const ReachPlay after = sheathReachEnded(reach, 0, items);
    CHECK_FALSE(after.play[0]);
    CHECK(reach.drawing[0]);
}

TEST_CASE("from melee to a gun: away, then a second reach draws it (0x007367b0)", "[attachment][reach]") {
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2};
    const UnitWeaponItem shield{.sheath = 4, .inventoryType = 14, .itemClass = 4};
    const UnitWeaponItem gun{.sheath = 1, .inventoryType = 26, .itemClass = 2};
    const UnitWeaponItems items{&sword, &shield, &gun};
    ReachPlay play;
    SheathReach reach = beginSheathReach(SheathState::Melee, SheathState::Ranged, items, play);
    CHECK(play.play[0]);
    CHECK(play.play[1]);
    sheathReachSwap(reach, 0, items);
    sheathReachSwap(reach, 1, items);
    CHECK_FALSE(reach.shown.mainHeld);
    CHECK_FALSE(reach.shown.offHeld);
    CHECK(reach.shown.ranged == RangedShown::None);
    // The right hand's reach ends and the gun, put at its point, is reached for.
    ReachPlay second = sheathReachEnded(reach, 0, items);
    CHECK(second.play[0]);
    CHECK(second.animId[0] == kAnimSheath);
    CHECK(reach.shown.ranged == RangedShown::Away);
    sheathReachSwap(reach, 0, items);
    CHECK(reach.shown.ranged == RangedShown::Held);
    // The left hand has nothing of the ranged state's.
    second = sheathReachEnded(reach, 1, items);
    CHECK_FALSE(second.play[1]);
}

TEST_CASE("from a bow to melee the right hand draws at once (0x00736b60)", "[attachment][reach]") {
    const UnitWeaponItem sword{.sheath = 1, .inventoryType = 17, .itemClass = 2};
    const UnitWeaponItem bow{.sheath = 2, .inventoryType = 15, .itemClass = 2};
    const UnitWeaponItems items{&sword, nullptr, &bow};
    ReachPlay play;
    SheathReach reach = beginSheathReach(SheathState::Ranged, SheathState::Melee, items, play);
    // The left hand puts the bow away; the right draws the sword alongside.
    CHECK(play.play[1]);
    CHECK_FALSE(reach.drawing[1]);
    CHECK(play.play[0]);
    CHECK(reach.drawing[0]);
    CHECK(reach.shown.ranged == RangedShown::Held);
    sheathReachSwap(reach, 1, items);
    CHECK(reach.shown.ranged == RangedShown::Away);
    // From ranged the main hand is dressed for the new state (0x0072dbc0).
    CHECK(reach.shown.mainHeld);
    // The right hand does not move the bow.
    reach.shown.ranged = RangedShown::Held;
    reach.drawing[0] = false;
    sheathReachSwap(reach, 0, items);
    CHECK(reach.shown.ranged == RangedShown::Held);
}

TEST_CASE("from unarmed to a bow: put at its point, drawn by the left hand", "[attachment][reach]") {
    const UnitWeaponItem bow{.sheath = 2, .inventoryType = 15, .itemClass = 2};
    const UnitWeaponItems items{nullptr, nullptr, &bow};
    ReachPlay play;
    SheathReach reach = beginSheathReach(SheathState::Unarmed, SheathState::Ranged, items, play);
    CHECK_FALSE(play.play[0]);
    CHECK(play.play[1]);
    CHECK(reach.shown.ranged == RangedShown::Away);
    sheathReachSwap(reach, 1, items);
    CHECK(reach.shown.ranged == RangedShown::Held);
}

TEST_CASE("a reach's weapons are dressed where it left them", "[attachment][reach]") {
    const UnitWeaponItem sword{.sheath = 3, .inventoryType = 13, .itemClass = 2};
    const UnitWeaponItem bow{.sheath = 2, .inventoryType = 15, .itemClass = 2};
    const UnitWeaponItems items{&sword, nullptr, &bow};
    UnitWeaponDress dress{.state = SheathState::Melee, .isPlayer = true};
    dress.reachShown = WeaponsShown{.mainHeld = false, .ranged = RangedShown::Away};
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, items, dress) == at::kHipWeaponLeft);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, items, dress) == at::kLargeWeaponRight);
    dress.reachShown = WeaponsShown{.mainHeld = true, .ranged = RangedShown::None};
    CHECK(unitWeaponPoint(WeaponSlot::MainHand, items, dress) == at::kHandRight);
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, items, dress) == at::kNone);
    dress.reachShown = WeaponsShown{.ranged = RangedShown::Held};
    CHECK(unitWeaponPoint(WeaponSlot::Ranged, items, dress) == at::kHandLeft);
}
