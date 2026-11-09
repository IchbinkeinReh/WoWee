// A model's object effects (0x006f7270, 0x006f61d0, 0x00721300, 0x006f17f0).
#include <catch_amalgamated.hpp>

#include "game/object_effect.hpp"

namespace oe = wowee::game::object_effect;

namespace {
// A siege engine's package: moving (0x25) and stationary (0x26) both loop the
// engine; moving also loops the fast gear and shifts once on the way in.
oe::Tables siege() {
    oe::Tables t;
    t.package[7] = {{0x25, 100}, {0x26, 101}};
    t.group[100] = {{1, 100, oe::kTriggerLoop, 1, 5000, 0, {}, 0},
                    {2, 100, oe::kTriggerLoop, 1, 5001, 0, {}, 0},
                    {3, 100, oe::kTriggerOnEnter, 1, 5002, 0, {}, 0}};
    t.group[101] = {{1, 101, oe::kTriggerLoop, 1, 5000, 0, {}, 0},
                    {4, 101, oe::kTriggerOnLeave, 1, 5003, 0, {}, 0}};
    return t;
}
}  // namespace

TEST_CASE("states start loops once and share them", "[object_effect]") {
    const oe::Tables t = siege();
    oe::Runner r;
    std::vector<oe::Action> out;
    r.enter(t, 7, 0x26, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == oe::Action::Kind::StartLoop);
    CHECK(out[0].effect->id == 1);
    out.clear();
    // Entering again is nothing.
    r.enter(t, 7, 0x26, out);
    CHECK(out.empty());
    // Moving: the shift once, the gear loop; the engine is already held.
    r.enter(t, 7, 0x25, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == oe::Action::Kind::PlayOnce);
    CHECK(out[0].effect->id == 3);
    CHECK(out[1].effect->id == 2);
    out.clear();
    // Leaving stationary: its leave sound; the engine stays, moving holds it.
    r.leave(t, 7, 0x26, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].effect->id == 4);
    out.clear();
    r.leave(t, 7, 0x25, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == oe::Action::Kind::StopLoop);
    CHECK(out[1].kind == oe::Action::Kind::StopLoop);
}

TEST_CASE("movement flags name the states (0x00721300)", "[object_effect]") {
    oe::States wanted, scope;
    oe::movementStates(0x1u | 0x2000000u | 0x400000u, false, true, wanted, scope);
    CHECK(wanted[0x25]);
    CHECK_FALSE(wanted[0x26]);
    CHECK(wanted[6]);
    CHECK_FALSE(wanted[7]);
    CHECK(wanted[0x30]);
    CHECK(wanted[0x3d]);
    CHECK(wanted[0x3e]);
    CHECK_FALSE(wanted[0x37]);
    CHECK(scope[0x42]);

    oe::States still, s2;
    oe::movementStates(0x8u | 0x10u, false, true, still, s2);
    CHECK(still[0x2f]);
    CHECK(still[0x37]);
    CHECK(still[0x3c]);
    CHECK(still[0x3b]);
    CHECK_FALSE(still[0x3a]);
}

TEST_CASE("animations name states (0x006f17f0)", "[object_effect]") {
    CHECK(oe::animationState(0, 0) == 19);
    CHECK(oe::animationState(0, 1) == 41);
    CHECK(oe::animationState(0, 2) == 19);
    CHECK(oe::animationState(5, 1) == 0);   // no second column for Run
    CHECK(oe::animationState(1, 0) == 15);
    CHECK(oe::animationState(5, 0) == 12);
    CHECK(oe::animationState(162, 0) == 68);
    CHECK(oe::animationState(2, 0) == 0);
    CHECK(oe::animationScope()[70]);
    CHECK(oe::endsMovementStates(15));
}

TEST_CASE("the speed sets a loop's pitch (0x006f1790)", "[object_effect]") {
    const oe::Modifier m{1, 1, 1, {5.0f, 30.0f, 0.7f, 1.3f}};
    float pitch = 0.0f;
    REQUIRE(oe::modifiedPitch(m, 0.0f, pitch));
    CHECK(pitch == Catch::Approx(0.7f));
    oe::modifiedPitch(m, 17.5f, pitch);
    CHECK(pitch == Catch::Approx(1.0f));
    oe::modifiedPitch(m, 40.0f, pitch);
    CHECK(pitch == Catch::Approx(1.3f));
    CHECK_FALSE(oe::modifiedPitch(oe::Modifier{2, 1, 1, {}}, 1.0f, pitch));
}

TEST_CASE("a moving object's four states, one at a time (0x0070b390, 0x007bd5a0)", "[object_effect]") {
    oe::Tables t;
    t.package[12] = {{3, 1}, {4, 2}, {5, 3}};
    t.group[1] = {{10, 1, oe::kTriggerOnEnter, 1, 900, 0, {}, 0}};
    t.group[2] = {{11, 2, oe::kTriggerLoop, 1, 901, 0, {}, 0}};
    t.group[3] = {{12, 3, oe::kTriggerOnEnter, 1, 902, 0, {}, 0}};
    oe::Runner r;
    std::vector<oe::Action> out;
    oe::moveState(r, t, 12, oe::routeMoveState(0xa2), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].effect->id == 10);
    out.clear();
    oe::moveState(r, t, 12, oe::routeMoveState(0xa3), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == oe::Action::Kind::StartLoop);
    out.clear();
    oe::moveState(r, t, 12, oe::routeMoveState(0xa4), out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].effect->id == 12);
    CHECK(out[1].kind == oe::Action::Kind::StopLoop);
    CHECK(r.states()[5]);
    CHECK_FALSE(r.states()[4]);
    CHECK(oe::routeMoveState(0) == 2);
    CHECK(oe::routeMoveState(7) == 0);
}

TEST_CASE("a unit's modifier input is the base's -1 (0x006f1700)", "[object_effect]") {
    const oe::Modifier m{1, 1, 1, {5.0f, 30.0f, 0.7f, 1.3f}};
    float pitch = 0.0f;
    REQUIRE(oe::modifiedPitch(m, oe::kUnitModifierInput, pitch));
    CHECK(pitch == Catch::Approx(0.7f));
}
