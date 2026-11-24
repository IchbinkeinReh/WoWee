// What the mount under a rider plays: what its rider's unit is asked
// (0x007385c0 handing it what 0x0071d6b0 says is a mount's), as that unit
// jumps off a pier, falls, swims and gets out again. A mounted fall into
// water used to leave the mount in its Jump for good: its own state machine
// waited for ground that a swimmer never touches.

#include <catch_amalgamated.hpp>

#include "game/entity.hpp"
#include "rendering/animation/jump_landing.hpp"
#include "rendering/animation/mover_event.hpp"
#include "rendering/animation/rider_mount.hpp"

#include <set>

namespace a = wowee::rendering::anim;
namespace ub = wowee::rendering::upper_body;
namespace ba = wowee::rendering::base_animation;
namespace rm = wowee::rendering::rider_mount;
namespace me = wowee::rendering::mover_event;
using wowee::game::Opcode;

namespace {

constexpr uint32_t kStand = 0, kWalk = 4, kRun = 5, kShuffleLeft = 11, kWalkBackwards = 13, kReady1H = 26,
                   kJumpStart = 37, kJump = 38, kJumpEnd = 39, kFall = 40, kSwimIdle = 41, kSwim = 42, kSwimLeft = 43,
                   kSwimRight = 44, kSwimBackwards = 45, kMount = 91, kFly = 135, kSprint = 143, kJumpLandRun = 187;

constexpr uint32_t kForward = 0x1, kBackward = 0x2, kStrafeLeft = 0x4, kStrafeRight = 0x8, kFalling = 0x1000,
                   kFallingFar = 0x2000, kAscending = 0x400000, kSwimming = 0x200000, kFlying = 0x2000000;

// AnimationData.dbc's rows for what a mount plays; the rest fall back to
// Stand, as most do.
a::AnimationDataTable table() {
    std::vector<a::AnimationDataRecord> rows;
    for (uint32_t id = 0; id < 506; ++id) rows.push_back({.id = id, .flags = 0, .fallback = 0, .behavior = id, .tier = 0});
    const auto row = [&rows](uint32_t id, uint32_t flags, uint32_t fallback) {
        rows[id].flags = flags;
        rows[id].fallback = fallback;
    };
    row(kStand, 0x3, 147);
    row(147, 0x3, 147);
    row(kWalkBackwards, 0x11, kWalk);
    row(kFall, 0x3, kSwimIdle);
    row(kSwim, 0x6, kWalk);
    row(kSwimBackwards, 0x16, kSwim);
    row(kFly, 0x2, kSwim);
    row(kSprint, 0x4, kRun);
    row(kJumpLandRun, 0x1, kRun);
    return a::AnimationDataTable(std::move(rows));
}

// A ground mount of 3.3.5: every one of them has the swims.
const std::set<uint32_t> kHorse{kStand, 1, kWalk, kRun, kShuffleLeft, 12, kWalkBackwards, kJumpStart, kJump, kJumpEnd,
                                kFall, kSwimIdle, kSwim, kSwimLeft, kSwimRight, kSwimBackwards, 94};

// A rider on its mount, as CharacterRenderer puts the pieces together.
struct Ride {
    a::AnimationDataTable data = table();
    std::set<uint32_t> model = kHorse;
    ub::Unit unit;
    uint32_t mount = kStand;
    bool loop = true;
    uint32_t pose = kMount;

    Ride() {
        unit.riding = true;
        unit.mounted = true;
        unit.speed = 14.0f;
        unit.walkSpeed = 2.5f;
    }
    [[nodiscard]] std::function<bool(uint32_t)> has() const {
        return [this](uint32_t id) { return model.count(id) != 0; };
    }
    /// 0x007385c0: the mount, and the rider's own body in its pose.
    void ask(uint32_t anim, bool loops) {
        const ub::Slots now{{static_cast<int32_t>(pose), 0.0f, true}, {}};
        const ub::Decision d = ub::decide(data, unit, {static_cast<int32_t>(anim), 0.0f, loops}, now);
        CHECK(d.body.anim == static_cast<int32_t>(kMount));
        if (d.toMount && mount != anim) {
            mount = anim;
            loop = loops;
        }
    }
    /// 0x0073ac30.
    void base() {
        const ba::Pick p = ba::pick(data, unit, {static_cast<int32_t>(pose), static_cast<int32_t>(pose)}, {}, has(), has());
        if (p.anim == ba::kKeep) return;
        ask(p.anim, p.loop);
        if (rm::takes(data, p.anim)) return;
        const ba::Pick m = rm::base(data, unit, 0, has(), has());
        if (m.anim != ba::kKeep) ask(m.anim, m.loop);
    }
    /// 0x0073ed10, and for a landing 0x0073d2b0.
    void event(Opcode op, uint32_t flags) {
        const uint32_t before = unit.moveFlags;
        const bool launched = unit.leaping && (before & kFallingFar) == 0;
        unit.moveFlags = flags;
        unit.leaping = (flags & kFalling) != 0 && op == Opcode::MSG_MOVE_JUMP;
        if (op == Opcode::MSG_MOVE_FALL_LAND) {
            switch (wowee::rendering::landed(before, flags, launched, unit.speed > 2.0f * unit.walkSpeed)) {
                case wowee::rendering::Landed::Nothing: break;
                case wowee::rendering::Landed::Base: base(); break;
                case wowee::rendering::Landed::End: ask(kJumpEnd, false); break;
                case wowee::rendering::Landed::LandRun: ask(kJumpLandRun, false); break;
            }
            return;
        }
        switch (me::react(op, flags, mount == kFall, false).play) {
            case me::Play::Nothing: break;
            case me::Play::Base: base(); break;
            case me::Play::JumpStart: ask(kJumpStart, false); break;
            case me::Play::Fall: ask(kFall, true); break;
            case me::Play::JumpLandRun: ask(kJumpLandRun, false); break;
        }
    }
    /// 0x0073ad00.
    void fallsFar() {
        unit.moveFlags |= kFalling | kFallingFar;
        unit.leaping = true;
        ask(kFall, true);
    }
    /// 0x0073bff0.
    void oneShotEnds() {
        REQUIRE_FALSE(loop);
        const ba::Pick p = rm::afterOneShot(data, unit, mount, has(), has());
        REQUIRE(p.anim != ba::kKeep);
        mount = p.anim;
        loop = p.loop;
    }
    /// 0x00826350: the sequence its model shows for it.
    [[nodiscard]] uint32_t shown() const { return a::modelAnimation(data, mount, has(), kStand).id; }
};

// Off the pier at a run, jumping, down past where the jump began.
void jumpOffThePier(Ride& r) {
    r.event(Opcode::MSG_MOVE_START_FORWARD, kForward);
    CHECK(r.mount == kSprint);
    CHECK(r.shown() == kRun);
    r.event(Opcode::MSG_MOVE_JUMP, kForward | kFalling);
    CHECK(r.mount == kJumpStart);
    CHECK_FALSE(r.loop);
    r.oneShotEnds();
    CHECK(r.mount == kJump);
    CHECK(r.loop);
    r.fallsFar();
    CHECK(r.mount == kFall);
}

}  // namespace

TEST_CASE("a mount that falls into water swims", "[rider_mount]") {
    Ride r;
    jumpOffThePier(r);
    // MSG_MOVE_START_SWIM, the fall over (0x00989660 -> 0x00988490).
    r.unit.speed = 4.7222f;
    r.event(Opcode::MSG_MOVE_START_SWIM, kForward | kSwimming);
    CHECK(r.mount == kSwim);
    CHECK(r.loop);
    r.event(Opcode::MSG_MOVE_STOP, kSwimming);
    CHECK(r.mount == kSwimIdle);
    r.event(Opcode::MSG_MOVE_START_BACKWARD, kBackward | kSwimming);
    CHECK(r.mount == kSwimBackwards);
    r.event(Opcode::MSG_MOVE_STOP, kSwimming);
    r.event(Opcode::MSG_MOVE_START_STRAFE_LEFT, kStrafeLeft | kSwimming);
    CHECK(r.mount == kSwimLeft);
    r.event(Opcode::MSG_MOVE_STOP_STRAFE, kSwimming);
    CHECK(r.mount == kSwimIdle);
    r.event(Opcode::MSG_MOVE_START_STRAFE_RIGHT, kStrafeRight | kSwimming);
    CHECK(r.mount == kSwimRight);
    r.event(Opcode::MSG_MOVE_STOP_STRAFE, kSwimming);
    // Straight up with no direction held it idles on.
    r.event(Opcode::MSG_MOVE_START_ASCEND, kAscending | kSwimming);
    CHECK(r.mount == kSwimIdle);
}

TEST_CASE("a mount that lands in water without a direction held idles in it", "[rider_mount]") {
    Ride r;
    jumpOffThePier(r);
    r.event(Opcode::MSG_MOVE_STOP, kFalling | kFallingFar);
    r.event(Opcode::MSG_MOVE_START_SWIM, kSwimming);
    CHECK(r.mount == kSwimIdle);
}

TEST_CASE("a jump so short it meets the water in JumpStart swims too", "[rider_mount]") {
    Ride r;
    r.event(Opcode::MSG_MOVE_JUMP, kFalling);
    CHECK(r.mount == kJumpStart);
    r.event(Opcode::MSG_MOVE_START_SWIM, kSwimming);
    CHECK(r.mount == kSwimIdle);
    CHECK(r.loop);
}

TEST_CASE("walked off the edge, the mount falls once it falls far", "[rider_mount]") {
    Ride r;
    r.event(Opcode::MSG_MOVE_START_FORWARD, kForward);
    // Half a second of the run in the air, then Fall, then the water.
    CHECK(r.mount == kSprint);
    r.fallsFar();
    CHECK(r.mount == kFall);
    r.event(Opcode::MSG_MOVE_START_SWIM, kForward | kSwimming);
    CHECK(r.mount == kSwim);
}

TEST_CASE("out of the water the mount walks or jumps again", "[rider_mount]") {
    Ride r;
    r.event(Opcode::MSG_MOVE_START_SWIM, kForward | kSwimming);
    CHECK(r.mount == kSwim);
    // Up the beach.
    r.event(Opcode::MSG_MOVE_STOP_SWIM, kForward);
    CHECK(r.mount == kSprint);
    r.event(Opcode::MSG_MOVE_STOP, 0);
    CHECK(r.mount == kStand);
    // Back in, and out with a jump onto the pier: MSG_MOVE_JUMP alone.
    r.event(Opcode::MSG_MOVE_START_SWIM, kForward | kSwimming);
    r.event(Opcode::MSG_MOVE_JUMP, kForward | kFalling);
    CHECK(r.mount == kJumpStart);
    r.event(Opcode::MSG_MOVE_FALL_LAND, kForward);
    CHECK(r.mount == kJumpLandRun);
    CHECK(r.shown() == kRun);
    r.oneShotEnds();
    CHECK(r.mount == kSprint);
}

TEST_CASE("a mount lands standing with JumpEnd and back into its stand", "[rider_mount]") {
    Ride r;
    r.event(Opcode::MSG_MOVE_JUMP, kFalling);
    r.oneShotEnds();
    CHECK(r.mount == kJump);
    r.event(Opcode::MSG_MOVE_FALL_LAND, 0);
    CHECK(r.mount == kJumpEnd);
    CHECK_FALSE(r.loop);
    r.oneShotEnds();
    CHECK(r.mount == kStand);
    CHECK(r.loop);
}

TEST_CASE("a model without the swims plays what AnimationData falls back to", "[rider_mount]") {
    Ride r;
    r.model = {kStand, kWalk, kRun};
    r.event(Opcode::MSG_MOVE_START_SWIM, kForward | kSwimming);
    CHECK(r.mount == kSwim);
    CHECK(r.shown() == kWalk);
    r.event(Opcode::MSG_MOVE_START_BACKWARD, kBackward | kSwimming);
    CHECK(r.shown() == kWalk);
    CHECK(a::modelAnimation(r.data, r.mount, r.has(), kStand).mode == a::PlayMode::Reversed);
    r.event(Opcode::MSG_MOVE_STOP, kSwimming);
    CHECK(r.mount == kSwimIdle);
    CHECK(r.shown() == kStand);
    // Falling, with no Fall: SwimIdle's, else Stand.
    r.fallsFar();
    CHECK(r.shown() == kStand);
    r.model.insert(kSwimIdle);
    CHECK(r.shown() == kSwimIdle);
}

TEST_CASE("a flier swims the air (0x00717050)", "[rider_mount]") {
    Ride r;
    r.event(Opcode::MSG_MOVE_START_FORWARD, kForward | kFlying);
    CHECK(r.mount == kSwim);
    r.event(Opcode::MSG_MOVE_STOP, kFlying);
    CHECK(r.mount == kSwimIdle);
    // Down from it.
    r.event(Opcode::CMSG_MOVE_SET_FLY, 0);
    CHECK(r.mount == kJumpLandRun);
    // Along a flight path's spline it flies.
    r.unit.splineFlying = true;
    r.event(Opcode::MSG_MOVE_START_FORWARD, kForward);
    CHECK(r.mount == kFly);
}

TEST_CASE("a flier out of the water by a jump flies on", "[rider_mount]") {
    Ride r;
    r.event(Opcode::MSG_MOVE_START_FORWARD, kForward | kFlying);
    r.event(Opcode::MSG_MOVE_START_SWIM, kForward | kSwimming);
    CHECK(r.mount == kSwim);
    r.event(Opcode::MSG_MOVE_JUMP, kForward | kFalling);
    CHECK(r.mount == kJumpStart);
    // Up in the air the fall is over (0x00989890).
    r.event(Opcode::MSG_MOVE_START_ASCEND, kForward | kAscending | kFlying);
    CHECK(r.mount == kSwim);
    r.event(Opcode::MSG_MOVE_STOP, kFlying);
    CHECK(r.mount == kSwimIdle);
}

TEST_CASE("what is the rider's own leaves the mount to its own", "[rider_mount]") {
    Ride r;
    r.event(Opcode::MSG_MOVE_START_FORWARD, kForward);
    // Stopping in a fight: the ready is the rider's arms', the mount stands.
    r.unit.autoAttacking = true;
    r.unit.meleeReady = kReady1H;
    r.event(Opcode::MSG_MOVE_STOP, 0);
    CHECK(r.mount == kStand);
    // In the water it idles there.
    r.event(Opcode::MSG_MOVE_START_SWIM, kSwimming);
    CHECK(r.mount == kSwimIdle);
    // A bow left out, an emote state: none of the mount's.
    r.unit.autoAttacking = false;
    r.unit.rangedDrawn = r.unit.autoRepeat = true;
    r.event(Opcode::MSG_MOVE_STOP_SWIM, 0);
    CHECK(r.mount == kStand);
    r.unit.rangedDrawn = r.unit.autoRepeat = false;
    r.unit.emoteStateAnim = 69;
    r.event(Opcode::MSG_MOVE_START_FORWARD, kForward);
    CHECK(r.mount == kSprint);
    r.event(Opcode::MSG_MOVE_STOP, 0);
    CHECK(r.mount == kStand);
}
