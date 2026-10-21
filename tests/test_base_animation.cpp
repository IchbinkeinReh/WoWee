// The animation a unit is in when nothing else is asked of it (0x00724500)
// and the combat ready its weapons give it (0x00714dd0).
#include <catch_amalgamated.hpp>

#include "rendering/animation/base_animation.hpp"

#include <set>

namespace a = wowee::rendering::anim;
namespace ub = wowee::rendering::upper_body;
namespace ba = wowee::rendering::base_animation;

namespace {

a::AnimationDataTable table() {
    std::vector<a::AnimationDataRecord> rows;
    for (uint32_t id = 0; id < 506; ++id) rows.push_back({.id = id, .flags = 0, .fallback = 0, .behavior = id, .tier = 0});
    return a::AnimationDataTable(std::move(rows));
}

struct Model {
    std::set<uint32_t> ids{0};
    [[nodiscard]] bool has(uint32_t id) const { return ids.count(id) != 0; }
};

ba::Pick pick(const ub::Unit& u, ba::Playing playing = {0, 0}, ba::Ask ask = {}, const Model& model = {}) {
    const auto has = [&model](uint32_t id) { return model.has(id); };
    return ba::pick(table(), u, playing, ask, has, has);
}

constexpr uint32_t kStand = 0, kDead = 6, kWalk = 4, kRun = 5, kShuffleLeft = 11, kShuffleRight = 12,
                   kWalkBackwards = 13, kAttack1H = 17, kReadyUnarmed = 25, kReady1H = 26, kReady2H = 27,
                   kReady2HL = 28, kSpellCast = 32, kJumpLoop = 38, kFall = 40, kSwimIdle = 41, kSwim = 42,
                   kSwimLeft = 43, kSwimBackwards = 45, kEmoteWork = 62, kSitGroundDown = 96, kSitGround = 97,
                   kSleep = 100, kKneelLoop = 115, kStealthWalk = 119, kStealthStand = 120, kDrowned = 132,
                   kSprint = 143, kHover = 193, kSubmerged = 202, kDeathStart = 466, kDeathEndHold = 472;

}  // namespace

TEST_CASE("the ready for the weapons in hand (0x00714dd0)", "[base_animation]") {
    const ba::HandItem sword{2, 7}, greatsword{2, 8}, staff{2, 10}, bow{2, 2}, shield{4, 6};
    CHECK(ba::meleeReady(nullptr, nullptr) == kReadyUnarmed);
    CHECK(ba::meleeReady(&sword, nullptr) == kReady1H);
    CHECK(ba::meleeReady(&greatsword, nullptr) == kReady2H);
    // A two-hander with something in the off hand: the one-hand ready.
    CHECK(ba::meleeReady(&greatsword, &greatsword) == kReady1H);
    CHECK(ba::meleeReady(&staff, nullptr) == kReady2HL);
    CHECK(ba::meleeReady(&bow, nullptr) == kReadyUnarmed);
    // No weapon in the main hand: the off hand's, where that is one.
    CHECK(ba::meleeReady(&shield, &sword) == kReady1H);
    CHECK(ba::meleeReady(&shield, nullptr) == kReadyUnarmed);
}

TEST_CASE("dead: the death's end held, or Dead, Drowned in water", "[base_animation]") {
    ub::Unit u;
    u.alive = false;
    CHECK(pick(u).anim == kDead);
    u.moveFlags = ub::kMoveSwimming;
    CHECK(pick(u).anim == kDrowned);
    u.moveFlags = 0;
    // Dying goes on.
    CHECK(pick(u, {1, 1}).anim == ba::kKeep);
    // Feigning death, or lying dead, is dead too.
    ub::Unit feign;
    feign.feignDeath = true;
    CHECK(pick(feign).anim == kDead);
    ub::Unit lying;
    lying.standState = 7;
    CHECK(pick(lying).anim == kDead);
    // A model with the death start: its held end, or the start falling.
    Model m;
    m.ids = {0, kDeathStart};
    CHECK(pick(u, {0, 0}, {}, m).anim == kDeathEndHold);
    u.moveFlags = ub::kMoveFalling;
    const auto p = pick(u, {0, 0}, {}, m);
    CHECK(p.anim == kDeathStart);
    CHECK_FALSE(p.loop);
    // Falling far without one it is left as it is.
    u.moveFlags = ub::kMoveFalling | 0x2000;
    CHECK(pick(u).anim == ba::kKeep);
}

TEST_CASE("in the air: the jump goes on, else the fall", "[base_animation]") {
    ub::Unit u;
    u.leaping = true;
    CHECK(pick(u).anim == kFall);
    CHECK(pick(u, {kJumpLoop, kJumpLoop}).anim == ba::kKeep);
}

TEST_CASE("moving: walk, run, sprint by the speed; back, stealth, swim", "[base_animation]") {
    ub::Unit u;
    u.moveFlags = 0x1;
    u.walkSpeed = 2.5f;
    u.speed = 2.5f;
    CHECK(pick(u).anim == kWalk);
    u.speed = 5.0f;
    CHECK(pick(u).anim == kWalk);  // twice the walk speed is still a walk
    u.speed = 7.0f;
    CHECK(pick(u).anim == kRun);
    u.speed = 11.5f;
    CHECK(pick(u).anim == kSprint);
    u.stealthed = true;
    CHECK(pick(u).anim == kStealthWalk);
    u.moveFlags = 0x2;
    CHECK(pick(u).anim == kWalkBackwards);
    u.moveFlags = 0x1 | ub::kMoveSwimming;
    CHECK(pick(u).anim == kSwim);
    u.moveFlags = 0x2 | ub::kMoveSwimming;
    CHECK(pick(u).anim == kSwimBackwards);
    u.moveFlags = 0x4 | ub::kMoveFlying;
    CHECK(pick(u).anim == kSwimLeft);
    // Moving comes before the fight.
    u.moveFlags = 0x1;
    u.stealthed = false;
    u.speed = 7.0f;
    u.autoAttacking = true;
    CHECK(pick(u).anim == kRun);
}

TEST_CASE("fighting: the swing that waited, the swim idle, the ready", "[base_animation]") {
    ub::Unit u;
    u.autoAttacking = true;
    u.meleeReady = kReady1H;
    CHECK(pick(u).anim == kReady1H);
    const auto queued = pick(u, {0, 0}, {.queuedAttack = static_cast<int32_t>(kAttack1H)});
    CHECK(queued.anim == kAttack1H);
    CHECK_FALSE(queued.loop);
    u.moveFlags = ub::kMoveSwimming;
    CHECK(pick(u).anim == kSwimIdle);
    // Not with neither the body nor the upper body free.
    u.moveFlags = 0;
    CHECK(pick(u, {0, 0}, {.free = ba::kMountFree}).anim == kStand);
}

TEST_CASE("turning on the spot: the shuffle its way", "[base_animation]") {
    ub::Unit u;
    u.turningInPlace = true;
    CHECK(pick(u).anim == kShuffleRight);
    u.turningLeft = true;
    CHECK(pick(u).anim == kShuffleLeft);
    ub::Unit turning;
    turning.moveFlags = 0x10;  // turning left
    CHECK(pick(turning).anim == kShuffleLeft);
    // Not with a cast or an emote playing, nor hovering or swimming.
    CHECK(pick(u, {kSpellCast, 0}).anim == kStand);
    u.hovering = true;
    CHECK(pick(u).anim == kHover);
}

TEST_CASE("the stand state's loop, the emote state, the stand", "[base_animation]") {
    ub::Unit u;
    u.standState = 1;
    CHECK(pick(u).anim == kSitGround);
    // The sit-down still going goes on.
    const auto down = pick(u, {kSitGroundDown, kSitGroundDown});
    CHECK(down.anim == kSitGroundDown);
    CHECK_FALSE(down.loop);
    u.standState = 3;
    CHECK(pick(u).anim == kSleep);
    u.standState = 8;
    CHECK(pick(u).anim == kKneelLoop);
    // Not riding, nor with the body taken.
    u.riding = true;
    CHECK(pick(u).anim == kStand);
    u.riding = false;
    CHECK(pick(u, {0, 0}, {.free = ba::kUpperFree}).anim == kStand);
    // Submerged where the model has it.
    ub::Unit sub;
    sub.standState = 9;
    Model m;
    m.ids = {0, kSubmerged};
    CHECK(pick(sub, {0, 0}, {}, m).anim == kSubmerged);
    CHECK(pick(sub, {static_cast<int32_t>(kSubmerged), 0}, {}, m).anim == ba::kKeep);
    CHECK(pick(sub).anim == kStand);
    // The emote state's animation.
    ub::Unit worker;
    worker.emoteStateAnim = kEmoteWork;
    CHECK(pick(worker).anim == kEmoteWork);
    // And at the end: swim idle, stealth stand, hover, stand.
    ub::Unit idle;
    CHECK(pick(idle).anim == kStand);
    idle.hovering = true;
    CHECK(pick(idle).anim == kHover);
    idle.stealthed = true;
    CHECK(pick(idle).anim == kStealthStand);
    idle.moveFlags = ub::kMoveFlying;
    CHECK(pick(idle).anim == kSwimIdle);
    // A settle goes on; a seated ride is the seat's.
    ub::Unit settled;
    CHECK(pick(settled, {464, 464}).anim == 464);
    settled.seatedRide = true;
    CHECK(pick(settled).anim == ba::kKeep);
}

TEST_CASE("casting: the kit's loop (0x00724330)", "[base_animation]") {
    constexpr uint32_t kReadySpellDirected = 51, kChannelCastDirected = 124;
    ub::Unit u;
    u.castTimed = true;
    u.castLoopAnim = kReadySpellDirected;
    u.castLoopKitFlags = 0x8;
    auto p = pick(u);
    CHECK(p.anim == kReadySpellDirected);
    // Kit Flags 0x8: not put in it by the tier.
    CHECK(p.flags == 0x4u);
    // Already the body's: left as it is - but asked again from the body's
    // end with kit Flags 0x40.
    const ba::Playing body{static_cast<int32_t>(kReadySpellDirected), static_cast<int32_t>(kReadySpellDirected)};
    CHECK(pick(u, body).anim == ba::kKeep);
    CHECK(pick(u, body, {.free = ba::kBodyFree, .flags = 0x10}).anim == ba::kKeep);
    u.castLoopKitFlags = 0x40;
    CHECK(pick(u, body, {.free = ba::kBodyFree, .flags = 0x10}).anim == kReadySpellDirected);
    // On the upper body: kept there.
    CHECK(pick(u, {static_cast<int32_t>(kReadySpellDirected), 0}).anim == kReadySpellDirected);
    // A channel's loop, before a melee target.
    ub::Unit channel;
    channel.channeling = true;
    channel.autoAttacking = true;
    channel.castLoopAnim = kChannelCastDirected;
    CHECK(pick(channel).anim == kChannelCastDirected);
    // Not riding, nor with no cast going.
    channel.riding = true;
    CHECK(pick(channel).anim != kChannelCastDirected);
    ub::Unit idle;
    idle.castLoopAnim = kReadySpellDirected;
    CHECK(pick(idle).anim == kStand);
}
