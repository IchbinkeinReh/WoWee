// The body yaw CGUnit's animation update draws a unit at (FUN_0073dab0), its
// spring (FUN_00719660) and the shuffle it asks for turning on the spot.
#include <catch_amalgamated.hpp>
#include "rendering/animation/body_yaw.hpp"

#include <utility>

using namespace wowee::rendering::body_yaw;
using Catch::Approx;

namespace {
constexpr float kDt = 1.0f / 60.0f;

Input standing(float facing) {
    Input in;
    in.facing = facing;
    in.dt = kDt;
    in.catchUpSeconds = kDt;
    in.turnRate = kPi;
    in.hasSpine = true;
    in.hasHead = true;
    return in;
}
} // namespace

TEST_CASE("wrap keeps an angle within half a turn", "[body_yaw]") {
    CHECK(wrap(0.5f) == Approx(0.5f));
    CHECK(wrap(kPi + 0.5f) == Approx(-kPi + 0.5f));
    CHECK(wrap(-kPi - 0.5f) == Approx(kPi - 0.5f));
    CHECK(std::abs(wrap(5.0f * kPi)) == Approx(kPi).margin(1e-5));
}

TEST_CASE("Standing, the body catches up at eight times the turn rate", "[body_yaw]") {
    State s;
    s.body = 0.0f;
    const Input in = standing(1.0f);
    const Result r = update(s, in);
    // pi * 8 a second for one 60th: 0.4189 rad.
    CHECK(r.step == Approx(kPi * 8.0f * kDt));
    CHECK(s.body == Approx(kPi * 8.0f * kDt));
    // What is left shows on the spine first, the whole of it for the player.
    CHECK(r.spineYaw == Approx(1.0f - kPi * 8.0f * kDt).margin(1e-5));
    CHECK(r.headYaw == Approx(0.0f).margin(1e-5));
    // In a few frames it is there, and nothing is left to show.
    Result last;
    for (int i = 0; i < 4; ++i) last = update(s, in);
    CHECK(s.body == Approx(1.0f));
    CHECK(last.spineYaw == 0.0f);
    CHECK(last.headYaw == 0.0f);
}

TEST_CASE("Turned by the player, the body holds and is dragged past a quarter turn", "[body_yaw]") {
    State s;
    s.body = 0.0f;
    Input in = standing(1.0f);
    in.inputTurning = true;
    Result r = update(s, in);
    CHECK(r.step == 0.0f);
    CHECK(s.body == 0.0f);
    // Spine first, up to an eighth of a turn; the head takes the rest.
    CHECK(r.spineYaw == Approx(kQuarterPi));
    CHECK(r.headYaw == Approx(1.0f - kQuarterPi));

    // Past a quarter turn the body comes along, never further behind.
    in.facing = 2.0f;
    r = update(s, in);
    CHECK(r.step == Approx(2.0f - kHalfPi));
    CHECK(wrap(in.facing - s.body) == Approx(kHalfPi));
    CHECK(r.spineYaw == Approx(kQuarterPi));
    CHECK(r.headYaw == Approx(kQuarterPi));
    CHECK(turnShuffle(0, r.step, true) == TurnShuffle::Left);

    // Turning right the other way round.
    State t;
    Input right = standing(-2.0f);
    right.inputTurning = true;
    r = update(t, right);
    CHECK(r.step == Approx(-(2.0f - kHalfPi)));
    CHECK(r.spineYaw == Approx(-kQuarterPi));
    CHECK(r.headYaw == Approx(-kQuarterPi));
}

TEST_CASE("Released, the body catches up the way it lags", "[body_yaw]") {
    State s;
    s.body = 0.0f;
    const Input in = standing(-kHalfPi);
    const Result r = update(s, in);
    CHECK(r.step < 0.0f);
    CHECK(turnShuffle(0, r.step, true) == TurnShuffle::Right);
}

TEST_CASE("Across the half turn the body goes the short way", "[body_yaw]") {
    State s;
    s.body = 3.0f;
    const Input in = standing(-3.0f);
    const Result r = update(s, in);
    CHECK(r.step > 0.0f);  // 3 -> -3 is 0.28 rad to the left
    CHECK(s.body == Approx(-3.0f).margin(1e-4));
}

TEST_CASE("Other units' bodies take the whole lag at once", "[body_yaw]") {
    State s;
    Input in = standing(1.0f);
    in.catchUpSeconds = 1.0e6f;  // +0xabc never stamped
    in.halveSpine = true;
    const Result r = update(s, in);
    CHECK(s.body == Approx(1.0f));
    CHECK(r.step == Approx(1.0f));
    CHECK(r.spineYaw == 0.0f);

    // Halved on the spine when there is a lag to share; the head the rest.
    State t;
    Input held = standing(0.0f);
    held.halveSpine = true;
    held.inputTurning = true;
    t.body = -1.0f;
    const Result h = update(t, held);
    CHECK(h.spineYaw == Approx(0.5f));
    CHECK(h.headYaw == Approx(0.5f));
}

TEST_CASE("Mounted the spine takes nothing and the head what it can", "[body_yaw]") {
    State s;
    Input in = standing(1.0f);
    in.inputTurning = true;
    in.spineAllowed = false;
    const Result r = update(s, in);
    CHECK(r.spineYaw == 0.0f);
    CHECK(r.headYaw == Approx(kQuarterPi));
}

TEST_CASE("A model with neither key bone, swimming or flying is drawn at the facing", "[body_yaw]") {
    State s;
    s.body = 0.0f;
    Input in = standing(2.0f);
    in.hasSpine = false;
    in.hasHead = false;
    const Result r = update(s, in);
    CHECK(s.body == Approx(2.0f));
    CHECK(r.step == 0.0f);

    State w;
    Input swim = standing(2.0f);
    swim.moveFlags = kSwimming;
    update(w, swim);
    CHECK(w.body == Approx(2.0f));

    State f;
    Input fly = standing(-2.0f);
    fly.moveFlags = kFlying;
    update(f, fly);
    CHECK(f.body == Approx(-2.0f));

    State d;
    Input dead = standing(1.0f);
    dead.snap = true;
    update(d, dead);
    CHECK(d.body == Approx(1.0f));
}

TEST_CASE("Strafing turns the body to the side it goes", "[body_yaw]") {
    auto settle = [](uint32_t flags) {
        State s;
        Input in = standing(0.0f);
        in.moveFlags = flags;
        Result r;
        for (int i = 0; i < 120; ++i) r = update(s, in);
        return std::pair<float, Result>(s.body, r);
    };
    // A quarter turn, held just inside it by the spring.
    const auto [left, lr] = settle(kStrafeLeft);
    CHECK(left == Approx(1.5704823f).margin(1e-3));
    // The upper body looks back round to the facing.
    CHECK(lr.spineYaw == Approx(-kQuarterPi));
    CHECK(lr.headYaw == Approx(-(1.5704823f - kQuarterPi)).margin(1e-3));
    const auto [right, rr] = settle(kStrafeRight);
    CHECK(right == Approx(-1.5704823f).margin(1e-3));
    CHECK(rr.spineYaw == Approx(kQuarterPi));
    // An eighth going forward too.
    CHECK(settle(kForward | kStrafeLeft).first == Approx(kQuarterPi).margin(1e-3));
    CHECK(settle(kForward | kStrafeRight).first == Approx(-kQuarterPi).margin(1e-3));
    // Backing off to the left the body turns right, as backing up turns the legs.
    CHECK(settle(kBackward | kStrafeLeft).first == Approx(-kQuarterPi).margin(1e-3));
    CHECK(settle(kBackward | kStrafeRight).first == Approx(kQuarterPi).margin(1e-3));
}

TEST_CASE("Moving off, the spring carries the body to the facing and lets go", "[body_yaw]") {
    State s;
    s.body = 0.0f;
    s.weight = 1.0f;
    Input in = standing(1.0f);
    in.moveFlags = kForward;
    in.inputTurning = true;
    const Result first = update(s, in);
    CHECK(s.body > 0.0f);
    CHECK(s.body < 1.0f);
    CHECK(first.step == 0.0f);
    // The weight runs out over 0.4 s; then the body is at the facing.
    for (int i = 0; i < 30; ++i) update(s, in);
    CHECK(s.weight <= 0.0f);
    CHECK(s.body == Approx(1.0f));

    // Standing again, the weight comes back for the next start.
    const Input stand = standing(1.0f);
    for (int i = 0; i < 30; ++i) update(s, stand);
    CHECK(s.weight >= 1.0f);
}

TEST_CASE("The spring holds the body within a quarter turn of the facing", "[body_yaw]") {
    State s;
    s.body = 3.0f;
    s.weight = 1.0f;
    spring(s, 0.0f, 0.0f, kDt);
    CHECK(std::abs(s.body) <= 1.5704823f);
    CHECK(s.body > 0.0f);
}

TEST_CASE("The shuffle: by key or by the body's turn, standing still", "[body_yaw]") {
    CHECK(turnShuffle(kTurnLeft, 0.0f, true) == TurnShuffle::Left);
    CHECK(turnShuffle(kTurnRight, 0.0f, true) == TurnShuffle::Right);
    CHECK(turnShuffle(0, 0.1f, true) == TurnShuffle::Left);
    CHECK(turnShuffle(0, -0.1f, true) == TurnShuffle::Right);
    CHECK(turnShuffle(0, 1e-6f, true) == TurnShuffle::None);
    CHECK(turnShuffle(0, 0.0f, true) == TurnShuffle::None);
    // Left first.
    CHECK(turnShuffle(kTurnLeft, -0.1f, true) == TurnShuffle::Left);
    CHECK(turnShuffle(kTurnRight, 0.1f, true) == TurnShuffle::Left);
    // Not moving, falling, swimming or flying; not sitting.
    CHECK(turnShuffle(kTurnLeft | kForward, 0.0f, true) == TurnShuffle::None);
    CHECK(turnShuffle(kTurnLeft | kStrafeRight, 0.0f, true) == TurnShuffle::None);
    CHECK(turnShuffle(kTurnLeft | kFalling, 0.0f, true) == TurnShuffle::None);
    CHECK(turnShuffle(kTurnLeft | kSwimming, 0.0f, true) == TurnShuffle::None);
    CHECK(turnShuffle(kTurnLeft | kFlying, 0.0f, true) == TurnShuffle::None);
    CHECK(turnShuffle(kTurnLeft, 0.0f, false) == TurnShuffle::None);
}
