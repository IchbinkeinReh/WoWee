// Which of ScenePick's answers a caller wants.
//
// The struct carries several, and two of them are easy to confuse: closestGuid
// is the nearest *entry point* of anything the ray touched, and resolve() is
// what a click acts on. They disagree exactly where it matters: a unit is
// entered by its sphere before the body is met, or is not.
//
// Nothing raises when a caller picks the wrong one. It just quietly describes a
// different thing from the one the click will hit.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <optional>
#include <vector>

#include "ui/scene_pick.hpp"

using wowee::ui::ScenePick;

namespace {
constexpr uint64_t kVendor = 0xF130000001u;
constexpr uint64_t kStall  = 0xF110000002u;
constexpr uint64_t kCorpse = 0xF130000004u;
}  // namespace

TEST_CASE("the triangles the ray meets first win the click") {
    // The vendor's body is in front of the stall's counter at the pointer.
    ScenePick pick;
    pick.closestGuid = kStall;
    pick.closestT = 8.0f;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 10.0f;
    pick.unitT = 9.6f;
    pick.objectGuid = kStall;
    pick.objectT = 10.5f;
    CHECK(pick.resolve() == kVendor);
    CHECK(pick.closestGuid != pick.resolve());

    // A mailbox between the player and an NPC behind it, or the counter in front of him.
    pick.objectT = 9.5f;
    CHECK(pick.resolve() == kStall);
    pick.objectGuid = 0;
    CHECK(pick.resolve() == kVendor);
}

TEST_CASE("a unit reached by its sphere alone is there where no object is") {
    ScenePick pick;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 10.0f;
    CHECK(pick.resolve() == kVendor);
    pick.objectGuid = kStall;
    pick.objectT = 30.0f;
    CHECK(pick.resolve() == kStall);
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

TEST_CASE("of two bodies lying on one another the triangles in front are picked") {
    // Two pack mobs fell on one another: the upper one's centre is nearer
    // along the ray, but the pointer is on what shows of the lower one, whose
    // triangles the ray meets first (0x004f9550 -> 0x0081df10 tests the
    // dead as the living). By the centres alone the lower one was never
    // under the pointer (the tester's corpse at 3 yards).
    constexpr uint64_t kUpper = 0xF130000010u;
    constexpr uint64_t kLower = 0xF130000011u;
    std::vector<wowee::ui::UnitCandidate> dead = {{kUpper, 3.0f, 2.0f}, {kLower, 3.4f, 2.2f}};
    const auto best = wowee::ui::frontUnitByTriangles(dead, [&](uint64_t guid, float reach) -> std::optional<float> {
        const float at = guid == kLower ? 2.6f : 2.9f;
        if (at > reach) return std::nullopt;
        return at;
    });
    REQUIRE(best.has_value());
    CHECK(dead[best->index].guid == kLower);
    CHECK(best->t == 2.6f);

    // The ray meets no body's triangles: the spheres decide.
    std::vector<wowee::ui::UnitCandidate> none = {{kUpper, 3.0f, 2.0f}, {kLower, 3.4f, 2.2f}};
    CHECK_FALSE(wowee::ui::frontUnitByTriangles(none, [](uint64_t, float) { return std::optional<float>{}; }));

    // One whose sphere starts past a body already met is not tested.
    int tested = 0;
    std::vector<wowee::ui::UnitCandidate> far = {{kUpper, 3.0f, 2.0f}, {kLower, 9.0f, 8.0f}};
    const auto front = wowee::ui::frontUnitByTriangles(far, [&](uint64_t guid, float) -> std::optional<float> {
        ++tested;
        return guid == kUpper ? std::optional<float>{2.5f} : std::optional<float>{8.5f};
    });
    REQUIRE(front.has_value());
    CHECK(far[front->index].guid == kUpper);
    CHECK(tested == 1);
}
