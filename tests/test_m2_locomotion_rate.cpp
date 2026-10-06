// The rate a unit's movement animation plays at.
//
// The 3.3.5a client plays a movement sequence at the unit's speed over the
// sequence's movingSpeed, so the feet keep to the ground: FUN_007385c0 sets
// the rate (CMovement::GetCurrentSpeed / |movingSpeed|) for the ids
// FUN_00714e80 calls movement, carries the stride from one moving sequence to
// the next in integer arithmetic, and FUN_00826c40 leaves the sequence being
// blended out running at the rate it had. Without it every creature walked
// and ran at the sequence's own pace whatever its speed, and the feet slid.
//
// The oracle is the client's arithmetic as decompiled.
#include <catch_amalgamated.hpp>

#include <vector>

#include "rendering/m2_track_sampler.hpp"

using namespace wowee::rendering::m2_track;
using wowee::pipeline::M2Sequence;

TEST_CASE("The client's movement animations", "[m2][locomotion]") {
    // FUN_00714e80's switch, all seventeen.
    for (uint32_t id : {4u, 5u, 11u, 12u, 13u, 37u, 38u, 39u, 42u, 43u, 44u, 45u,
                        119u, 135u, 143u, 187u, 223u}) {
        CHECK(isLocomotionAnimation(id));
    }
    // Stand, Death, SwimIdle, FlyIdle (158), Mount are not.
    for (uint32_t id : {0u, 1u, 41u, 91u, 158u}) {
        CHECK_FALSE(isLocomotionAnimation(id));
    }
}

TEST_CASE("A movement sequence plays at the unit's speed over its movingSpeed",
          "[m2][locomotion]") {
    // A human Run authored at 7 yards a second, played by a unit at 7: as authored.
    CHECK(locomotionPlaybackRate(5, 7.0f, 1.0f, 7.0f) == Catch::Approx(1.0f));
    // The same run under a 30% speed buff plays 30% faster.
    CHECK(locomotionPlaybackRate(5, 7.0f, 1.0f, 9.1f) == Catch::Approx(1.3f));
    // A creature on a slow spline walks slower than its Walk.
    CHECK(locomotionPlaybackRate(4, 2.5f, 1.0f, 1.25f) == Catch::Approx(0.5f));
    // A model drawn at twice the size covers twice the ground a stride
    // (FUN_0082ced0 multiplies movingSpeed by the scale), so it strides half as fast.
    CHECK(locomotionPlaybackRate(5, 7.0f, 2.0f, 7.0f) == Catch::Approx(0.5f));
    // The client takes the magnitude: a negative movingSpeed plays forward.
    CHECK(locomotionPlaybackRate(13, -4.5f, 1.0f, 4.5f) == Catch::Approx(1.0f));
}

TEST_CASE("Anything else plays as authored", "[m2][locomotion]") {
    // Not moving.
    CHECK(locomotionPlaybackRate(5, 7.0f, 1.0f, 0.0f) == 1.0f);
    // Not a movement animation, even with a movingSpeed: Stand, an attack.
    CHECK(locomotionPlaybackRate(0, 7.0f, 1.0f, 7.0f) == 1.0f);
    CHECK(locomotionPlaybackRate(16, 7.0f, 1.0f, 7.0f) == 1.0f);
    // A movement animation whose sequence has no movingSpeed.
    CHECK(locomotionPlaybackRate(42, 0.0f, 1.0f, 4.72f) == 1.0f);
    // Nothing usable.
    CHECK(locomotionPlaybackRate(5, 7.0f, 1.0f, std::nanf("")) == 1.0f);
    CHECK(locomotionPlaybackRate(5, 7.0f, 0.0f, 7.0f) == 1.0f);
}

TEST_CASE("Changing movement sequence carries the stride over", "[m2][locomotion]") {
    // Three quarters through an 800ms Walk is three quarters through a 600ms Run.
    CHECK(locomotionPhaseTime(600.0f, 800, 600) == Catch::Approx(450.0f));
    // Integer arithmetic, as the client's MUL/DIV: 333 * 1000 / 700 = 475.
    CHECK(locomotionPhaseTime(333.9f, 700, 1000) == Catch::Approx(475.0f));
    // Wrapped into the new sequence's length.
    CHECK(locomotionPhaseTime(1500.0f, 1000, 1000) == Catch::Approx(500.0f));
    // No length on either side: from the start.
    CHECK(locomotionPhaseTime(400.0f, 0, 1000) == 0.0f);
    CHECK(locomotionPhaseTime(400.0f, 1000, 0) == 0.0f);
}

TEST_CASE("The sequence blended out keeps the rate it had", "[m2][locomotion][blend]") {
    std::vector<M2Sequence> seqs(2);
    seqs[0].duration = 1000;
    seqs[1].duration = 1000;
    seqs[1].blendTime = 200;
    SequenceBlend blend;
    // Leaving a Run playing at 1.5x at 100ms into it.
    beginSequenceBlend(blend, 0.0f, 0, 100.0f, true, false, 1, seqs[1].blendTime, seqs, 1.5f);
    // 100ms of clock later it is 150ms further on, not 100.
    const BlendSample s = currentBlend(blend, 100.0f, seqs, 1, 100.0f);
    CHECK(s.sequenceIndex == 0);
    CHECK(s.timeMs == Catch::Approx(250.0f));
    CHECK(s.weight == Catch::Approx(0.5f));
}
