// Click-to-move: the reach per kind (0x0071b3b0), the turn (0x00731650) and
// the frame step (0x007317a0).
#include <catch_amalgamated.hpp>
#include "game/click_to_move.hpp"
#include "game/spell_book_records.hpp"

using namespace wowee::game::click_to_move;
using Catch::Matchers::WithinAbs;

namespace {
constexpr float kDeg = 0.017453292f;

StepIn standingAt(const glm::vec3& pos, float facing) {
    StepIn in;
    in.pos = pos;
    in.facing = facing;
    in.dtSeconds = 0.016f;
    return in;
}
}  // namespace

TEST_CASE("each kind has its own reach", "[click_to_move]") {
    ReachInput in;
    CHECK(*reachSq(Kind::Move, in) == 0.25f);
    CHECK(*reachSq(Kind::Follow, in) == 9.0f);
    // An NPC: half its combat reach and two yards; gone, (25/9)^2.
    CHECK_THAT(*reachSq(Kind::NpcInteract, in), WithinAbs((25.0 / 9.0) * (25.0 / 9.0), 1e-4));
    in.targetCombatReach = 1.5f;
    CHECK_THAT(*reachSq(Kind::NpcInteract, in), WithinAbs(2.75 * 2.75, 1e-5));
    // Loot and melee: the melee reach, at least five, less four thirds.
    in.moverCombatReach = 1.5f;
    CHECK_THAT(*reachSq(Kind::Loot, in), WithinAbs((5.0 - 4.0 / 3.0) * (5.0 - 4.0 / 3.0), 1e-4));
    in.targetCombatReach = 5.0f;
    CHECK_THAT(*reachSq(Kind::AttackPosition, in), WithinAbs(6.5 * 6.5, 1e-4));
    in.targetCombatReach.reset();
    CHECK(*reachSq(Kind::Loot, in) == 25.0f);
    // An object: the reach the last object approach left behind.
    in.objectReach = 4.5f;
    CHECK_THAT(*reachSq(Kind::ObjectInteract, in), WithinAbs(20.25, 1e-5));
    // Skinning and shooting: nine tenths of the spell's range, or no start.
    CHECK_FALSE(reachSq(Kind::AttackGuid, in));
    in.spellRange = 30.0f;
    CHECK_THAT(*reachSq(Kind::AttackGuid, in), WithinAbs(27.0 * 27.0, 1e-3));
    CHECK_THAT(*reachSq(Kind::Skin, in), WithinAbs(27.0 * 27.0, 1e-3));
    // The kinds that never arrive want none.
    CHECK(*reachSq(Kind::Face, in) == 0.0f);
    CHECK(*reachSq(Kind::FaceDirection, in) == 0.0f);
}

TEST_CASE("the melee reach is never under five yards", "[click_to_move]") {
    CHECK(meleeReach(0.5f, 1.0f) == 5.0f);
    CHECK_THAT(meleeReach(3.0f, 1.5f), WithinAbs(5.8333333, 1e-5));
}

TEST_CASE("a kind with a limit does not start from past it", "[click_to_move]") {
    const glm::vec3 me(0.0f);
    CHECK(startsInRange(Kind::Follow, me, glm::vec3(29.0f, 0.0f, 0.0f)));
    CHECK_FALSE(startsInRange(Kind::Follow, me, glm::vec3(30.0f, 0.0f, 0.0f)));
    CHECK(startsInRange(Kind::NpcInteract, me, glm::vec3(79.0f, 0.0f, 0.0f)));
    CHECK_FALSE(startsInRange(Kind::NpcInteract, me, glm::vec3(0.0f, 0.0f, 80.0f)));
    CHECK(startsInRange(Kind::Move, me, glm::vec3(5000.0f, 0.0f, 0.0f)));
}

TEST_CASE("a turn goes at 800 degrees a second, a big one at 1200", "[click_to_move]") {
    CHECK_THAT(turnRateFor(90.0f * kDeg), WithinAbs(800.0f * kDeg, 1e-4));
    CHECK_THAT(turnRateFor(180.0f * kDeg), WithinAbs(1200.0f * kDeg, 1e-4));
    CHECK_THAT(turnRateFor(300.0f * kDeg), WithinAbs(800.0f * kDeg, 1e-4));
}

TEST_CASE("a turn takes the short way by at most a step", "[click_to_move]") {
    float rate = kTurnRateUnset;
    Turn t = turnToward(0.0f, kPi * 0.5f, 0.1f, rate, false, kPi);
    CHECK(t.turning);
    CHECK_THAT(t.facing, WithinAbs(0.1, 1e-5));
    CHECK_THAT(rate, WithinAbs(800.0f * kDeg, 1e-4));
    // Chosen once: the next turn keeps it.
    turnToward(0.0f, kPi, 0.1f, rate, false, kPi);
    CHECK_THAT(rate, WithinAbs(800.0f * kDeg, 1e-4));
    rate = kTurnRateUnset;
    t = turnToward(0.0f, kPi * 1.5f, 0.1f, rate, false, kPi);
    CHECK_THAT(t.facing, WithinAbs(kTwoPi - 0.1, 1e-5));
    // The last bit of a turn is not overshot, and a facing already there
    // is no turn.
    t = turnToward(1.0f, 1.05f, 0.1f, rate, false, kPi);
    CHECK_THAT(t.facing, WithinAbs(1.05, 1e-5));
    CHECK_FALSE(turnToward(1.0f, 1.0005f, 0.1f, rate, false, kPi).turning);
    // A mover that turns at its own rate uses it.
    rate = kTurnRateUnset;
    turnToward(0.0f, 1.0f, 0.1f, rate, true, 2.0f);
    CHECK(rate == 2.0f);
}

TEST_CASE("a move faces its place, goes, and arrives within half a yard", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    StepOut out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    CHECK_FALSE(out.facing);
    CHECK(out.forward == true);
    CHECK((s.flags & kFlagFacing) != 0);
    // Under way, nothing to change.
    in.movingAny = in.movingForwardBack = true;
    in.pos = glm::vec3(5.0f, 0.0f, 0.0f);
    out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    CHECK_FALSE(out.forward);
    in.pos = glm::vec3(9.6f, 0.0f, 0.0f);
    CHECK(step(s, in).outcome == Outcome::Arrived);
}

TEST_CASE("a move's arrival widens with the speed past seven", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(9.2f, 0.0f, 0.0f), 0.0f);
    in.currentSpeed = 7.0f;
    CHECK(step(s, in).outcome == Outcome::Continue);
    in.currentSpeed = 14.0f;  // a yard
    CHECK(step(s, in).outcome == Outcome::Arrived);
}

TEST_CASE("the place is steered toward while going", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(0.0f, 10.0f, 0.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.dtSeconds = 0.05f;
    const StepOut out = step(s, in);
    REQUIRE(out.facing);
    // The first frame turns at the sentinel rate, pi a second.
    CHECK_THAT(*out.facing, WithinAbs(kPi * 0.05f, 1e-5));
    CHECK(out.forward == true);
    CHECK((s.flags & kFlagFacing) == 0);
}

TEST_CASE("a face stops once faced, without moving", "[click_to_move]") {
    State s = start(Kind::Face, 0, glm::vec3(0.0f), 1.0f, 0.0f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.dtSeconds = 0.01f;
    StepOut out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    CHECK(out.facing);
    CHECK_FALSE(out.forward);
    in.facing = 1.0f;
    CHECK(step(s, in).outcome == Outcome::Stop);
}

TEST_CASE("an approach gives up eighty yards out, flat unless swimming or flying", "[click_to_move]") {
    State s = start(Kind::Loot, 7, glm::vec3(0.0f), 0.0f, 9.0f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.subject = glm::vec3(10.0f, 0.0f, 100.0f);
    CHECK(step(s, in).outcome == Outcome::Continue);
    in.swimOrFly = true;
    CHECK(step(s, in).outcome == Outcome::Stop);
    // A subject gone ends it.
    State t = start(Kind::NpcInteract, 7, glm::vec3(0.0f), 0.0f, 9.0f, glm::vec3(0.0f), false, kPi, 0);
    in.subject.reset();
    CHECK(step(t, in).outcome == Outcome::Stop);
}

TEST_CASE("a mover that stays put five frames running is stopped", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.movingAny = in.movingForwardBack = true;
    for (int i = 0; i < 4; ++i) CHECK(step(s, in).outcome == Outcome::Continue);
    CHECK(step(s, in).outcome == Outcome::Stop);
}

TEST_CASE("a mover that gets further away five frames running is stopped", "[click_to_move]") {
    State s = start(Kind::AttackPosition, 7, glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 1.0f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    step(s, in);  // faces it
    in.movingAny = in.movingForwardBack = true;
    float x = 0.0f;
    for (int i = 0; i < 4; ++i) {
        x -= 0.5f;
        in.pos = glm::vec3(x, 0.0f, 0.0f);
        CHECK(step(s, in).outcome == Outcome::Continue);
    }
    in.pos = glm::vec3(x - 0.5f, 0.0f, 0.0f);
    CHECK(step(s, in).outcome == Outcome::Stop);
}

TEST_CASE("steered by the mouse until it is behind, it is given up", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    step(s, in);
    in.movingAny = in.movingForwardBack = true;
    in.mouseSteering = true;
    in.pos = glm::vec3(0.5f, 0.0f, 0.0f);
    in.facing = 90.0f * kDeg;  // steered aside: kept
    StepOut out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    CHECK_FALSE(out.facing);
    in.pos = glm::vec3(1.0f, 0.0f, 0.0f);
    in.facing = 180.0f * kDeg;
    CHECK(step(s, in).outcome == Outcome::Stop);
}

TEST_CASE("a mover that may not move is stopped, not ended", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.movingAny = in.movingForwardBack = true;
    in.mayMove = false;
    const StepOut out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    CHECK(out.forward == false);
}

TEST_CASE("a follow starts moving only four and a half yards out", "[click_to_move]") {
    State s = start(Kind::Follow, 7, glm::vec3(0.0f), 0.0f, 9.0f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.groundSpeed = 7.0f;
    in.subject = glm::vec3(4.0f, 0.0f, 0.0f);
    StepOut out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    CHECK_FALSE(out.forward);
    in.subject = glm::vec3(5.0f, 0.0f, 0.0f);
    CHECK(step(s, in).forward == true);
    // Faster, further: twice the run speed wants nine yards.
    in.groundSpeed = 14.0f;
    in.subject = glm::vec3(8.0f, 0.0f, 0.0f);
    CHECK_FALSE(step(s, in).forward);
    // Within its three yards it has arrived.
    in.subject = glm::vec3(2.5f, 0.0f, 0.0f);
    CHECK(step(s, in).outcome == Outcome::Arrived);
}

TEST_CASE("arriving leaves the subject's action for the stop", "[click_to_move]") {
    Pending p;
    CHECK(arrive(Kind::NpcInteract, 5, p));
    CHECK(arrive(Kind::AttackPosition, 6, p));
    CHECK(arrive(Kind::Move, 0, p));
    CHECK_FALSE(arrive(Kind::Follow, 9, p));
    // One a stop, the attack first.
    Taken t = take(p);
    CHECK(t.action == Action::Attack);
    CHECK(t.guid == 6);
    t = take(p);
    CHECK(t.action == Action::Npc);
    CHECK(t.guid == 5);
    CHECK(take(p).action == Action::None);
}

TEST_CASE("the marker swings between half and three quarters of a yard", "[click_to_move]") {
    CHECK_THAT(markerRadius(0), WithinAbs(0.625, 1e-5));
    CHECK_THAT(markerRadius(125), WithinAbs(0.75, 1e-5));
    CHECK_THAT(markerRadius(375), WithinAbs(0.5, 1e-5));
    CHECK_THAT(markerRadius(500), WithinAbs(0.625, 1e-5));
}

TEST_CASE("a direction's facing", "[click_to_move]") {
    CHECK_THAT(facingOfDirection(glm::vec3(1.0f, 0.0f, 0.0f)), WithinAbs(0.0, 1e-6));
    CHECK_THAT(facingOfDirection(glm::vec3(0.0f, 1.0f, 0.0f)), WithinAbs(kPi * 0.5, 1e-6));
    CHECK_THAT(facingOfDirection(glm::vec3(0.0f, -1.0f, 0.0f)), WithinAbs(kPi * 1.5, 1e-6));
    CHECK_THAT(facingOfDirection(glm::vec3(-1.0f, 0.0f, 0.0f)), WithinAbs(kPi, 1e-6));
}

TEST_CASE("the spell book notes the last ranged attack learned", "[click_to_move]") {
    using wowee::game::spell_book::Records;
    using wowee::game::spell_book::Row;
    Records r;
    Row ranged;
    ranged.attrEx4 = 0x01000000u;
    r.learned(75, ranged);       // Auto Shot
    r.learned(133, Row{});       // nothing to note
    CHECK(r.rangedAttack == 75);
    r.learned(5019, ranged);     // Shoot, learned later
    CHECK(r.rangedAttack == 5019);
    // Forgetting the noted one leaves none, not the one before.
    r.removed(75);
    CHECK(r.rangedAttack == 5019);
    r.removed(5019);
    CHECK(r.rangedAttack == 0);
    r.learned(75, ranged);
    r.clear();
    CHECK(r.rangedAttack == 0);
}

TEST_CASE("the spell book notes a skinning spell per kind of corpse", "[click_to_move]") {
    using wowee::game::spell_book::Records;
    using wowee::game::spell_book::Row;
    Records r;
    Row skinning{0, 95, 0};
    Row herbs{0, 95, 1};
    Row mining{0, 95, 2};
    Row insignia{0, 116, 0};
    r.learned(8613, skinning);
    r.learned(2366, herbs);
    r.learned(2575, mining);
    r.learned(22027, insignia);
    CHECK(r.skin[0] == 8613);
    CHECK(r.skin[1] == 2366);
    CHECK(r.skin[2] == 2575);
    CHECK(r.skin[3] == 0);
    CHECK(r.skinPlayerCorpse == 22027);
    // A misc value past the four is noted nowhere.
    r.learned(1, Row{0, 95, 7});
    r.removed(2366);
    CHECK(r.skin[1] == 0);
    r.removed(22027);
    CHECK(r.skinPlayerCorpse == 0);
    CHECK(r.skin[0] == 8613);
}

TEST_CASE("a follow gives up thirty yards out, flat on the ground", "[click_to_move]") {
    State s = start(Kind::Follow, 7, glm::vec3(0.0f), 0.0f, 9.0f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    // Twenty-nine yards along and twenty up is within it on foot...
    in.subject = glm::vec3(29.0f, 0.0f, 20.0f);
    CHECK(step(s, in).outcome == Outcome::Continue);
    // ...not for a swimmer, whose distance counts the height.
    in.swimOrFly = true;
    CHECK(step(s, in).outcome == Outcome::Stop);
    State t = start(Kind::Follow, 7, glm::vec3(0.0f), 0.0f, 9.0f, glm::vec3(0.0f), false, kPi, 0);
    StepIn flat = standingAt(glm::vec3(0.0f), 0.0f);
    flat.subject = glm::vec3(30.0f, 0.0f, 0.0f);
    CHECK(step(t, flat).outcome == Outcome::Stop);
}

TEST_CASE("a spell's facing kinds turn to the unit and keep facing it", "[click_to_move]") {
    State s = start(Kind::Kind0, 7, glm::vec3(0.0f), 0.0f, 0.0f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.subject = glm::vec3(0.0f, 10.0f, 0.0f);
    StepOut out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    REQUIRE(out.facing);
    CHECK(*out.facing > 0.0f);
    CHECK_FALSE(out.forward);  // it goes nowhere
    // Facing it, it does not end as a plain turn does; it goes on facing it.
    in.facing = kPi * 0.5f;
    out = step(s, in);
    CHECK(out.outcome == Outcome::Continue);
    CHECK_FALSE(out.facing);
    CHECK_FALSE(out.forward);
    // Far away is no reason to give up: the kind has no limit.
    in.subject = glm::vec3(0.0f, 500.0f, 0.0f);
    CHECK(step(s, in).outcome == Outcome::Continue);
    // Its unit gone, it ends.
    in.subject.reset();
    CHECK(step(s, in).outcome == Outcome::Stop);
}

TEST_CASE("a swimmer or flyer pitches toward the place at pi a second", "[click_to_move]") {
    // Straight over: up; level: none; 30 degrees up.
    CHECK_THAT(pitchTo(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 5.0f)), WithinAbs(kPi * 0.5, 1e-6));
    CHECK_THAT(pitchTo(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -5.0f)), WithinAbs(-kPi * 0.5, 1e-6));
    CHECK_THAT(pitchTo(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f)), WithinAbs(0.0, 1e-6));
    CHECK_THAT(pitchTo(glm::vec3(0.0f), glm::vec3(std::sqrt(3.0f), 0.0f, 1.0f)), WithinAbs(kPi / 6.0, 1e-5));
    // A step at most, flagged far while more than 22.5 degrees off.
    PitchStep p = stepPitch(10.0f, 0.0f, 1.0f, 0.1f, 0.0f, std::nullopt, 1.5f);
    CHECK(p.pitching);
    CHECK_THAT(*p.pitch, WithinAbs(0.1, 1e-6));
    CHECK(p.farOff);
    p = stepPitch(10.0f, 0.0f, 0.15f, 0.1f, 0.1f, std::nullopt, 1.5f);
    CHECK_THAT(*p.pitch, WithinAbs(0.15, 1e-6));
    CHECK_FALSE(p.farOff);
    // There already: done, nothing set.
    p = stepPitch(10.0f, 0.0f, 0.15f, 0.1f, 0.1505f, std::nullopt, 1.5f);
    CHECK_FALSE(p.pitching);
    CHECK_FALSE(p.pitch);
    // Floating at the surface with the place above: levelled, and done.
    p = stepPitch(10.0f, 0.0f, 1.0f, 0.1f, 0.3f, 1.5f, 1.5f);
    CHECK_FALSE(p.pitching);
    REQUIRE(p.pitch);
    CHECK(*p.pitch == 0.0f);
}

TEST_CASE("a pitching swimmer holds still until near its pitch", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(10.0f, 0.0f, 10.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.swimOrFly = true;
    in.dtSeconds = 0.1f;
    const StepOut out = step(s, in);
    REQUIRE(out.pitch);
    CHECK_THAT(*out.pitch, WithinAbs(kPi * 0.1, 1e-5));
    // 45 degrees wanted, 18 reached: not yet forward.
    CHECK_FALSE(out.forward);
    in.pitch = kPi * 0.25f - 0.01f;
    const StepOut near = step(s, in);
    CHECK(near.forward == true);
}

TEST_CASE("one that can fly takes off for a steep place", "[click_to_move]") {
    State s = start(Kind::Move, 0, glm::vec3(2.0f, 0.0f, 20.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    StepIn in = standingAt(glm::vec3(0.0f), 0.0f);
    in.canFly = true;
    StepOut out = step(s, in);
    CHECK(out.takeOff);
    CHECK_FALSE(out.forward);
    // A shallow one is walked to.
    State t = start(Kind::Move, 0, glm::vec3(20.0f, 0.0f, 2.0f), 0.0f, 0.25f, glm::vec3(0.0f), false, kPi, 0);
    out = step(t, in);
    CHECK_FALSE(out.takeOff);
    CHECK(out.forward == true);
}

TEST_CASE("a direction clicked into while swimming gives its pitch", "[click_to_move]") {
    CHECK(pitchOfDirection(glm::vec3(1.0f, 0.0f, 0.1f)) == 0.0f);
    CHECK_THAT(pitchOfDirection(glm::vec3(0.0f, 0.0f, -1.0f)), WithinAbs(-kPi * 0.5, 1e-6));
    CHECK_THAT(pitchOfDirection(glm::vec3(0.0f, std::cos(0.5f), std::sin(0.5f))), WithinAbs(0.5, 1e-5));
}
