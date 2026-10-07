// Which of ScenePick's answers a caller wants.
//
// The struct carries several, and two of them are easy to confuse: closestGuid
// is the nearest *entry point* of anything the ray touched, and resolve() is
// what a click acts on. They disagree exactly where it matters. A game object's
// fallback sphere is 2.5 yards against a unit's 1.8, so a wide object beside an
// NPC is entered first even when the NPC's centre is nearer - which is why
// kMaxGameObjectPickRadius exists at all, and why the vendor cursor, asking
// closestGuid, lost every argument to the stall a merchant stands behind.
//
// Nothing raises when a caller picks the wrong one. It just quietly describes a
// different thing from the one the click will hit.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "ui/scene_pick.hpp"

using wowee::ui::ScenePick;

namespace {
constexpr uint64_t kVendor = 0xF130000001u;
constexpr uint64_t kStall  = 0xF110000002u;
constexpr uint64_t kWolf   = 0xF130000003u;
constexpr uint64_t kCorpse = 0xF130000004u;
}  // namespace

TEST_CASE("a unit behind a wider object still wins the click") {
    // The vendor's centre is nearer, but the stall's bigger sphere is entered
    // first - so closestGuid names the stall and resolve() names the vendor.
    ScenePick pick;
    pick.closestGuid = kStall;      // entered at 8.0
    pick.closestT = 8.0f;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 10.0f;
    pick.objectGuid = kStall;
    pick.objectCenterT = 10.5f;

    CHECK(pick.resolve() == kVendor);
    CHECK(pick.closestGuid != pick.resolve());
}

TEST_CASE("an object clearly in front of the unit takes it") {
    // Clearly is two yards. A mailbox between the player and an NPC behind it
    // is what the player means to click.
    ScenePick pick;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 14.0f;
    pick.objectGuid = kStall;
    pick.objectCenterT = 11.0f;
    CHECK(pick.resolve() == kStall);

    // Just inside the bias, and the unit keeps it.
    pick.objectCenterT = 12.5f;
    CHECK(pick.resolve() == kVendor);
}

TEST_CASE("a hostile outranks whatever else is under the cursor") {
    ScenePick pick;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 6.0f;
    pick.hostileUnitGuid = kWolf;
    pick.hostileUnitT = 9.0f;
    CHECK(pick.resolve() == kWolf);
}

TEST_CASE("a corpse is picked only when nothing living is there") {
    ScenePick pick;
    pick.deadUnitGuid = kCorpse;
    pick.deadUnitCenterT = 5.0f;
    CHECK(pick.resolve() == kCorpse);

    // Someone standing on the body takes it back, however the distances fall.
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 7.0f;
    CHECK(pick.resolve() == kVendor);
}

TEST_CASE("an empty pick resolves to nothing") {
    ScenePick pick;
    CHECK(pick.resolve() == 0u);
    CHECK(pick.unitGuid() == 0u);
}

// Which game objects the client lets the pointer rest on, and so names in a
// tooltip (0x0070f580 and the per-type classes behind it).
TEST_CASE("scenery the client does not track shows no tooltip") {
    using wowee::ui::gameObjectTakesMouseover;
    const uint32_t noData[24] = {};
    // A signpost or banner: generic, no highlight.
    CHECK_FALSE(gameObjectTakesMouseover(5, noData, 0, 0, false));
    CHECK_FALSE(gameObjectTakesMouseover(5, nullptr, 0, 0, false));
    // ...one whose template asks to be highlighted is tracked.
    uint32_t highlighted[24] = {};
    highlighted[1] = 1;
    CHECK(gameObjectTakesMouseover(5, highlighted, 0, 0, false));
    // A capture point keeps its highlight in data19.
    uint32_t capture[24] = {};
    CHECK_FALSE(gameObjectTakesMouseover(29, capture, 0, 0, false));
    capture[19] = 1;
    CHECK(gameObjectTakesMouseover(29, capture, 0, 0, false));
    // Transports, map objects, trap doors: never.
    for (uint32_t t : {11u, 14u, 15u, 31u, 35u})
        CHECK_FALSE(gameObjectTakesMouseover(t, noData, 0, 0, false));
}

TEST_CASE("an anvil is named though nothing uses it") {
    using wowee::ui::gameObjectTakesMouseover;
    using wowee::ui::gameObjectTakesClick;
    // Spell focus, duel arbiter, fishing hole, aura generator.
    for (uint32_t t : {8u, 16u, 25u, 30u}) {
        CHECK(gameObjectTakesMouseover(t, nullptr, 0x10, 0x4, false));
        CHECK_FALSE(gameObjectTakesClick(t, 0, 0));
    }
}

TEST_CASE("usable objects are tracked only while usable") {
    using wowee::ui::gameObjectTakesMouseover;
    CHECK(gameObjectTakesMouseover(3, nullptr, 0, 0, false));          // chest
    CHECK_FALSE(gameObjectTakesMouseover(3, nullptr, 0x10, 0, false)); // not selectable
    CHECK_FALSE(gameObjectTakesMouseover(3, nullptr, 0x1, 0, false));  // in use
    CHECK_FALSE(gameObjectTakesMouseover(10, nullptr, 0, 0x4, false)); // no-interact
    CHECK_FALSE(gameObjectTakesMouseover(10, nullptr, 0x4, 0, false)); // conditional, unlit
    CHECK(gameObjectTakesMouseover(10, nullptr, 0x4, 0x1, false));     // ...lit
    // A fishing bobber: only the player's own.
    CHECK_FALSE(gameObjectTakesMouseover(17, nullptr, 0, 0, false));
    CHECK(gameObjectTakesMouseover(17, nullptr, 0, 0, true));
}

TEST_CASE("the quest an object is kept for comes from its type's data field") {
    using wowee::ui::gameObjectRequiredQuest;
    uint32_t data[24] = {};
    for (uint32_t i = 0; i < 24; ++i) data[i] = 100 + i;
    CHECK(gameObjectRequiredQuest(3, data) == 108);   // chest data8
    CHECK(gameObjectRequiredQuest(5, data) == 105);   // generic data5
    CHECK(gameObjectRequiredQuest(8, data) == 104);   // spell focus data4
    CHECK(gameObjectRequiredQuest(10, data) == 101);  // goober data1
    CHECK(gameObjectRequiredQuest(19, data) == 0u);   // mailbox: none
    CHECK(gameObjectRequiredQuest(3, nullptr) == 0u);
}

TEST_CASE("the mouseover is the click's object, else the tracked one") {
    // What the game object highlight follows (0x0051f790): one object.
    ScenePick pick;
    CHECK(pick.mouseover() == 0u);
    // An anvil a click would not use is still the mouseover.
    pick.mouseoverObjectGuid = kStall;
    CHECK(pick.mouseover() == kStall);
    // A unit a click would act on takes it.
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 5.0f;
    CHECK(pick.mouseover() == kVendor);
}
