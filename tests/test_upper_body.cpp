// A unit's upper body on an animation of its own: whether it may differ
// from the body (0x00723e30) and where an animation asked goes (0x007385c0).
#include <catch_amalgamated.hpp>

#include "rendering/animation/upper_body.hpp"

namespace a = wowee::rendering::anim;
namespace ub = wowee::rendering::upper_body;

namespace {

// Ground rows whose BehaviorID is their id, as 3.3.5's are below 229.
a::AnimationDataTable table() {
    std::vector<a::AnimationDataRecord> rows;
    for (uint32_t id = 0; id < 506; ++id) rows.push_back({.id = id, .flags = 0, .fallback = 0, .behavior = id, .tier = 0});
    // FlyAttack1H: the swing's behaviour at the flying tier.
    rows[246] = {.id = 246, .flags = 0, .fallback = 245, .behavior = 17, .tier = 3};
    return a::AnimationDataTable(std::move(rows));
}

constexpr uint32_t kStand = 0, kDeath = 1, kWalk = 4, kRun = 5, kAttack1H = 17, kReady1H = 26, kSpellCast = 32,
                   kSwimIdle = 41, kSwim = 42, kSpellCastDirected = 53, kSpecial1H = 57, kEmoteTalk = 60,
                   kEmoteEat = 61, kEmoteWork = 62, kEmoteWave = 67, kMount = 91, kChannelCast = 124,
                   kFishing = 133;

ub::Unit running() {
    ub::Unit u;
    u.moveFlags = 0x1;  // forward
    return u;
}

ub::Slot asked(uint32_t anim, bool loop) { return ub::Slot{static_cast<int32_t>(anim), 0.0f, loop}; }

ub::Slots slots(int32_t body, float bodyTime = 0.0f, bool bodyLoop = true, int32_t upper = -1, float upperTime = 0.0f,
                bool upperLoop = false) {
    return ub::Slots{.body = {body, bodyTime, bodyLoop}, .upper = {upper, upperTime, upperLoop}};
}

}  // namespace

TEST_CASE("the animation lists 0x007385c0 tests", "[upper_body]") {
    CHECK(ub::isMovement(kRun));
    CHECK(ub::isMovement(143));       // Sprint
    CHECK_FALSE(ub::isMovement(kSwimIdle));
    CHECK(ub::isAttack(kAttack1H));
    CHECK(ub::isAttack(30));          // Dodge
    CHECK(ub::isSpell(kSpellCastDirected));
    CHECK_FALSE(ub::isSpell(kChannelCast));
    CHECK(ub::isUpperBody(kChannelCast));
    CHECK(ub::isUpperBody(kAttack1H));
    CHECK_FALSE(ub::isUpperBody(kRun));
    CHECK_FALSE(ub::isUpperBody(75));  // EmoteKneel
    CHECK(ub::isEmote(kEmoteWave));
    CHECK(ub::isDeath(kDeath));
    CHECK(ub::isDeath(6));
    CHECK(ub::isMountAnimation(kRun));
    CHECK_FALSE(ub::isMountAnimation(kSpellCast));
    CHECK(ub::isWholeBodySpecial(kSpecial1H));
}

TEST_CASE("an upper-body animation splits from the body on the move (0x00723e30)", "[upper_body]") {
    const auto t = table();
    CHECK(ub::splits(t, running(), kAttack1H, kRun));
    CHECK(ub::splits(t, running(), kEmoteTalk, kRun));
    // By its BehaviorID: the flight swing is a swing.
    CHECK(ub::splits(t, running(), 246, kRun));
    // Standing, it does not; nor with nothing playing; nor the run itself.
    CHECK_FALSE(ub::splits(t, ub::Unit{}, kAttack1H, kStand));
    CHECK_FALSE(ub::splits(t, running(), kAttack1H, -1));
    CHECK_FALSE(ub::splits(t, running(), kWalk, kRun));
    // Sitting, swimming, turning on the spot, leaping.
    ub::Unit sitting;
    sitting.standState = 1;
    CHECK(ub::splits(t, sitting, kSpellCast, 97));
    ub::Unit swimming;
    swimming.moveFlags = ub::kMoveSwimming;
    CHECK(ub::splits(t, swimming, kSpellCast, kSwimIdle));
    ub::Unit turning;
    turning.turningInPlace = true;
    CHECK(ub::splits(t, turning, kAttack1H, kStand));
    ub::Unit leaping;
    leaping.leaping = true;
    CHECK(ub::splits(t, leaping, kAttack1H, 38));
    // A ready splits only on a mount.
    CHECK_FALSE(ub::splits(t, ub::Unit{}, kReady1H, kStand));
    ub::Unit mounted;
    mounted.mounted = true;
    CHECK(ub::splits(t, mounted, kReady1H, kMount));
    // The jump over a swing; a swing falling.
    CHECK(ub::splits(t, ub::Unit{}, 37, kAttack1H));
    CHECK_FALSE(ub::splits(t, ub::Unit{}, 37, kStand));
    ub::Unit falling;
    falling.moveFlags = ub::kMoveFalling;
    CHECK(ub::splits(t, falling, kAttack1H, 38));
    // Casting with a cast time on the move, anything does.
    ub::Unit casting = running();
    casting.castTimed = true;
    CHECK(ub::splits(t, casting, kRun, kSpellCastDirected));
}

TEST_CASE("what never splits (0x00723e30)", "[upper_body]") {
    const auto t = table();
    CHECK_FALSE(ub::splits(t, running(), kDeath, kRun));
    CHECK_FALSE(ub::splits(t, running(), 39, kAttack1H));   // JumpEnd
    CHECK_FALSE(ub::splits(t, running(), 187, kAttack1H));  // JumpLandRun
    // Fishing kneeling.
    ub::Unit kneeling = running();
    kneeling.standState = 8;
    CHECK_FALSE(ub::splits(t, kneeling, kFishing, kRun));
    kneeling.standState = 1;
    CHECK(ub::splits(t, kneeling, kFishing, kRun));
    // Swinging, casting or channelling while turning, unless strafing.
    ub::Unit turning;
    turning.moveFlags = 0x10;  // turn left
    turning.autoAttacking = true;
    CHECK_FALSE(ub::splits(t, turning, kAttack1H, kStand));
    turning.moveFlags = 0x10 | 0x4;  // and strafing
    CHECK(ub::splits(t, turning, kAttack1H, kStand));
    turning.autoAttacking = false;
    turning.moveFlags = 0x10;
    CHECK(ub::splits(t, turning, kAttack1H, kStand));
}

TEST_CASE("a swing asked on the run goes to the upper body", "[upper_body]") {
    const auto t = table();
    const auto d = ub::decide(t, running(), asked(kAttack1H, false), slots(kRun));
    CHECK(d.upperAction == ub::Action::Play);
    CHECK(d.upper.anim == static_cast<int32_t>(kAttack1H));
    CHECK_FALSE(d.upper.loop);
    CHECK(d.bodyAction == ub::Action::Keep);
    CHECK(d.body.anim == static_cast<int32_t>(kRun));
}

TEST_CASE("running off mid-swing, the swing goes on above the legs", "[upper_body]") {
    const auto t = table();
    const auto d = ub::decide(t, running(), asked(kRun, true), slots(kAttack1H, 300.0f, false));
    CHECK(d.upperAction == ub::Action::Play);
    CHECK(d.upper.anim == static_cast<int32_t>(kAttack1H));
    CHECK(d.upper.timeMs == 300.0f);
    CHECK(d.upperFromBody);
    CHECK_FALSE(d.upperBlend);
    CHECK(d.bodyAction == ub::Action::Play);
    CHECK(d.body.anim == static_cast<int32_t>(kRun));
    CHECK(d.bodyBlend);
    // Mid-cast as mid-swing.
    const auto c = ub::decide(t, running(), asked(kRun, true), slots(kSpellCastDirected, 100.0f, true));
    CHECK(c.upper.anim == static_cast<int32_t>(kSpellCastDirected));
    CHECK(c.upper.loop);
    CHECK(c.body.anim == static_cast<int32_t>(kRun));
    // An emote is cut by the run.
    const auto e = ub::decide(t, running(), asked(kRun, true), slots(kEmoteWave, 100.0f, false));
    CHECK(e.upperAction == ub::Action::Keep);
    CHECK(e.body.anim == static_cast<int32_t>(kRun));
}

TEST_CASE("stopping mid-swing, the whole body takes the swing back", "[upper_body]") {
    const auto t = table();
    const auto d = ub::decide(t, ub::Unit{}, asked(kStand, true), slots(kRun, 0.0f, true, kAttack1H, 200.0f, false));
    CHECK(d.bodyAction == ub::Action::Play);
    CHECK(d.body.anim == static_cast<int32_t>(kAttack1H));
    CHECK(d.body.timeMs == 200.0f);
    CHECK(d.bodyFromUpper);
    CHECK(d.upperAction == ub::Action::Release);
    // The ready as the stand.
    const auto r = ub::decide(t, ub::Unit{}, asked(kReady1H, true), slots(kRun, 0.0f, true, kAttack1H, 200.0f, false));
    CHECK(r.body.anim == static_cast<int32_t>(kAttack1H));
    CHECK(r.upperAction == ub::Action::Release);
    // Walking on, the swing stays above.
    const auto w = ub::decide(t, running(), asked(kWalk, true), slots(kRun, 0.0f, true, kAttack1H, 200.0f, false));
    CHECK(w.body.anim == static_cast<int32_t>(kWalk));
    CHECK(w.upperAction == ub::Action::Keep);
    // A talk above the run is no swing: the stand leaves it there.
    const auto s = ub::decide(t, ub::Unit{}, asked(kStand, true), slots(kRun, 0.0f, true, kEmoteTalk, 0.0f, false));
    CHECK(s.body.anim == static_cast<int32_t>(kStand));
    CHECK(s.upperAction == ub::Action::Keep);
}

TEST_CASE("standing still, the whole body plays what it is asked", "[upper_body]") {
    const auto t = table();
    const auto d = ub::decide(t, ub::Unit{}, asked(kEmoteWave, false), slots(kStand));
    CHECK(d.bodyAction == ub::Action::Play);
    CHECK(d.body.anim == static_cast<int32_t>(kEmoteWave));
    CHECK(d.upperAction == ub::Action::Keep);
    CHECK(d.upper.anim == -1);
    // A one-shot asked again starts again (a swing over a swing waits).
    const auto again = ub::decide(t, ub::Unit{}, asked(kEmoteWave, false), slots(kEmoteWave, 400.0f, false));
    CHECK(again.bodyAction == ub::Action::Play);
    // A loop asked again goes on.
    const auto loop = ub::decide(t, ub::Unit{}, asked(kStand, true), slots(kStand, 400.0f, true));
    CHECK(loop.bodyAction == ub::Action::Keep);
}

TEST_CASE("EmoteEat always, EmoteWork swimming, go to the upper body", "[upper_body]") {
    const auto t = table();
    const auto eat = ub::decide(t, ub::Unit{}, asked(kEmoteEat, false), slots(kStand));
    CHECK(eat.upperAction == ub::Action::Play);
    CHECK(eat.upper.anim == static_cast<int32_t>(kEmoteEat));
    CHECK(eat.bodyAction == ub::Action::Keep);
    ub::Unit swimming;
    swimming.moveFlags = ub::kMoveSwimming;
    const auto work = ub::decide(t, swimming, asked(kEmoteWork, true), slots(kSwimIdle));
    CHECK(work.upperAction == ub::Action::Play);
    CHECK(work.bodyAction == ub::Action::Keep);
    // Standing, EmoteWork is the whole body's.
    CHECK(ub::decide(t, ub::Unit{}, asked(kEmoteWork, true), slots(kStand)).bodyAction == ub::Action::Play);
}

TEST_CASE("casting in place in the water, the body idles in it", "[upper_body]") {
    const auto t = table();
    ub::Unit u;
    u.moveFlags = ub::kMoveSwimming;
    u.castTimed = true;
    u.swimIdle = kSwimIdle;
    const auto d = ub::decide(t, u, asked(kSpellCast, false), slots(kSwim));
    CHECK(d.upper.anim == static_cast<int32_t>(kSpellCast));
    CHECK(d.body.anim == static_cast<int32_t>(kSwimIdle));
    CHECK(d.bodyAction == ub::Action::Play);
}

TEST_CASE("death, the whole-body specials and a whole-body model take the body", "[upper_body]") {
    const auto t = table();
    const auto death = ub::decide(t, running(), asked(kDeath, false), slots(kRun, 0.0f, true, kAttack1H));
    CHECK(death.body.anim == static_cast<int32_t>(kDeath));
    CHECK(death.bodyAction == ub::Action::Play);
    CHECK(death.upperAction == ub::Action::Release);
    const auto special = ub::decide(t, running(), asked(kSpecial1H, false), slots(kRun));
    CHECK(special.body.anim == static_cast<int32_t>(kSpecial1H));
    CHECK(special.upperAction == ub::Action::Keep);
    ub::Unit whole = running();
    whole.wholeBodyOnly = true;
    const auto w = ub::decide(t, whole, asked(kAttack1H, false), slots(kRun));
    CHECK(w.body.anim == static_cast<int32_t>(kAttack1H));
    CHECK(w.upperAction == ub::Action::Keep);
}

TEST_CASE("a rider's mount takes what moves, its upper body the rest", "[upper_body]") {
    const auto t = table();
    ub::Unit rider;
    rider.riding = true;
    rider.mounted = true;
    const auto cast = ub::decide(t, rider, asked(kSpellCastDirected, false), slots(kMount));
    CHECK(cast.upperAction == ub::Action::Play);
    CHECK(cast.upper.anim == static_cast<int32_t>(kSpellCastDirected));
    CHECK(cast.bodyAction == ub::Action::Keep);
    CHECK_FALSE(cast.toMount);
    const auto run = ub::decide(t, rider, asked(kRun, true), slots(kMount));
    CHECK(run.toMount);
    CHECK(run.upperAction == ub::Action::Keep);
    CHECK(run.bodyAction == ub::Action::Keep);
    // Off its pose, it is put back in it, straight in.
    const auto back = ub::decide(t, rider, asked(kRun, true), slots(kStand));
    CHECK(back.body.anim == static_cast<int32_t>(kMount));
    CHECK(back.bodyAction == ub::Action::Play);
    CHECK_FALSE(back.bodyBlend);
    // Death is the body's even so.
    CHECK(ub::decide(t, rider, asked(kDeath, false), slots(kMount)).body.anim == static_cast<int32_t>(kDeath));
}

TEST_CASE("the body's one-shot ended, only the body is asked (0x0073bbd0)", "[upper_body]") {
    const auto t = table();
    // Running with a swing above, the body's own one-shot over: the run.
    const auto run = ub::decide(t, running(), asked(kRun, true), slots(-1, 0.0f, true, kAttack1H, 100.0f, false),
                                ub::Pass::BodyEnded);
    CHECK(run.body.anim == static_cast<int32_t>(kRun));
    CHECK(run.upperAction == ub::Action::Keep);
    // An upper-body animation asked then goes to the body.
    const auto talk = ub::decide(t, running(), asked(kEmoteTalk, true), slots(-1), ub::Pass::BodyEnded);
    CHECK(talk.body.anim == static_cast<int32_t>(kEmoteTalk));
    CHECK(talk.upperAction == ub::Action::Keep);
}

TEST_CASE("seated on a ride animation, the upper body takes what is no stand (0x007385c0)", "[upper_body]") {
    const auto t = table();
    constexpr int32_t kRide = 400;  // the seat's ride animation, on the body
    ub::Unit seated;
    seated.seatedRide = true;
    // Always split, as 0x00723e30 answers first for a unit in its seat.
    CHECK(ub::splits(t, seated, kRun, kRide));
    CHECK(ub::splits(t, seated, kDeath, kRide));
    // A cast, a swing, an emote, a channel: the upper body's, the ride
    // animation keeping the body.
    for (uint32_t a : {kSpellCastDirected, kAttack1H, kEmoteWave, kChannelCast}) {
        const auto d = ub::decide(t, seated, asked(a, false), slots(kRide));
        CHECK(d.upperAction == ub::Action::Play);
        CHECK(d.upper.anim == static_cast<int32_t>(a));
        CHECK(d.bodyAction == ub::Action::Keep);
    }
    // So would the run be - the client never asks it of a unit whose own
    // movement flags carry no direction on its vehicle.
    CHECK(ub::decide(t, seated, asked(kRun, true), slots(kRide)).upper.anim == static_cast<int32_t>(kRun));
    // A stand: the upper body takes the body's, the ride animation.
    const auto stand = ub::decide(t, seated, asked(kStand, true), slots(kRide));
    CHECK(stand.upper.anim == kRide);
    CHECK(stand.upperFromBody);
    CHECK(stand.bodyAction == ub::Action::Keep);
    // Over a swing above, the stand leaves the swing there.
    const auto over = ub::decide(t, seated, asked(kStand, true), slots(kRide, 0.0f, true, kAttack1H, 100.0f, false));
    CHECK(over.upperAction == ub::Action::Keep);
    CHECK(over.bodyAction == ub::Action::Keep);
    // Dead in its seat, death is the body's.
    seated.alive = false;
    const auto death = ub::decide(t, seated, asked(kDeath, false), slots(kRide));
    CHECK(death.bodyAction == ub::Action::Play);
    CHECK(death.body.anim == static_cast<int32_t>(kDeath));
}

TEST_CASE("a swing asked over a swing waits, the one playing hastened (+0xb88)", "[upper_body]") {
    const auto data = table();
    ub::Unit standing;
    // On the body.
    auto d = ub::decide(data, standing, asked(kAttack1H, false), slots(kAttack1H, 200.0f, false));
    CHECK(d.queue);
    CHECK(d.bodyAction == ub::Action::Keep);
    CHECK(d.upperAction == ub::Action::Keep);
    // On the upper body, running.
    d = ub::decide(data, running(), asked(kAttack1H, false), slots(kRun, 0.0f, true, kAttack1H, 100.0f));
    CHECK(d.queue);
    CHECK(d.bodyAction == ub::Action::Keep);
    // Not once the body's one-shot has ended, nor over anything but a swing.
    d = ub::decide(data, standing, asked(kAttack1H, false), slots(kAttack1H, 0.0f, false), ub::Pass::BodyEnded);
    CHECK_FALSE(d.queue);
    d = ub::decide(data, standing, asked(kAttack1H, false), slots(kReady1H));
    CHECK_FALSE(d.queue);
    CHECK(d.bodyAction == ub::Action::Play);
    // Nor a cast asked over a swing.
    d = ub::decide(data, standing, asked(kSpellCast, false), slots(kAttack1H, 0.0f, false));
    CHECK_FALSE(d.queue);
}

TEST_CASE("the wound for a blow taken (0x00736640)", "[upper_body]") {
    const auto data = table();
    ub::Unit standing;
    // Standing still out of a fight: StandWound on the whole body.
    auto w = ub::wound(data, standing, false, kEmoteTalk, kEmoteTalk);
    REQUIRE(w);
    CHECK(w->anim == 8u);
    CHECK_FALSE(w->upper);
    // Running: on the upper body.
    w = ub::wound(data, running(), false, kRun, kRun);
    REQUIRE(w);
    CHECK(w->upper);
    // In a fight CombatWound, a critical CombatCritical - on the upper body
    // unless the body stands or is ready.
    ub::Unit fighting;
    fighting.autoAttacking = true;
    w = ub::wound(data, fighting, false, kAttack1H, kAttack1H);
    REQUIRE(w);
    CHECK(w->anim == 9u);
    CHECK(w->upper);
    w = ub::wound(data, fighting, true, kReady1H, kReady1H);
    REQUIRE(w);
    CHECK(w->anim == 10u);
    CHECK_FALSE(w->upper);
    w = ub::wound(data, standing, true, kStand, kStand);
    REQUIRE(w);
    CHECK_FALSE(w->upper);
    // Sitting, on the upper body.
    ub::Unit sitting;
    sitting.standState = 1;
    w = ub::wound(data, sitting, false, 97, 97);
    REQUIRE(w);
    CHECK(w->upper);
    // None dead or knocked down.
    ub::Unit dead;
    dead.alive = false;
    CHECK_FALSE(ub::wound(data, dead, false, kStand, kStand));
    CHECK_FALSE(ub::wound(data, standing, false, 121, 121));
}

TEST_CASE("hovering on the ground, the stand is the hover (0x0071df30)", "[upper_body]") {
    const auto data = table();
    ub::Unit hovering;
    hovering.hovering = true;
    // The model plays what it has: Stand for anything it lacks here.
    const auto plays = [](uint32_t id) { return id == kEmoteWave || id == kAttack1H ? id : kStand; };
    auto d = ub::decide(data, hovering, asked(kStand, true), slots(kEmoteWave, 0.0f, false));
    ub::applyHover(hovering, slots(kEmoteWave, 0.0f, false), d, plays);
    CHECK(d.bodyAction == ub::Action::Play);
    CHECK(d.body.anim == static_cast<int32_t>(ub::kHover));
    CHECK_FALSE(d.bodyBlend);
    // A stand-in that falls back to Stand is the hover too; what already
    // hovers goes on.
    d = ub::decide(data, hovering, asked(25, true), slots(ub::kHover));
    ub::applyHover(hovering, slots(ub::kHover), d, plays);
    CHECK(d.bodyAction == ub::Action::Keep);
    // Not a swing, nor swimming or flying.
    d = ub::decide(data, hovering, asked(kAttack1H, false), slots(kStand));
    ub::applyHover(hovering, slots(kStand), d, plays);
    CHECK(d.body.anim == static_cast<int32_t>(kAttack1H));
    ub::Unit flying = hovering;
    flying.moveFlags = ub::kMoveFlying;
    d = ub::decide(data, flying, asked(kStand, true), slots(kEmoteWave, 0.0f, false));
    ub::applyHover(flying, slots(kEmoteWave, 0.0f, false), d, plays);
    CHECK(d.body.anim == static_cast<int32_t>(kStand));
    CHECK(ub::hoverReplaces(8));
    CHECK_FALSE(ub::hoverReplaces(26));
}

TEST_CASE("a vehicle's upper body its seats animate is theirs (0x00756cd0)", "[upper_body]") {
    const auto data = table();
    ub::Unit vehicle = running();
    vehicle.vehicleHoldsUpper = true;
    // A swing on the move would go to the upper body: the bone is the
    // seat's, so nothing changes there.
    auto d = ub::decide(data, vehicle, asked(kAttack1H, false), slots(kRun, 0.0f, true, 160, 0.0f, true));
    CHECK(d.upperAction == ub::Action::Keep);
    // Stopping, the whole body does not take the upper body's back.
    ub::Unit still;
    still.vehicleHoldsUpper = true;
    d = ub::decide(data, still, asked(kStand, true), slots(kRun, 0.0f, true, kSpellCast, 100.0f, false));
    CHECK_FALSE(d.bodyFromUpper);
    CHECK(d.body.anim == static_cast<int32_t>(kStand));
    CHECK(d.upperAction == ub::Action::Keep);
    // Dying, it is not let go here.
    d = ub::decide(data, still, asked(kDeath, false), slots(kStand, 0.0f, true, 160, 0.0f, true));
    CHECK(d.upperAction == ub::Action::Keep);
}

TEST_CASE("a state kit animating the unit takes what is asked (0x0071e400)", "[upper_body]") {
    const auto data = table();
    constexpr uint32_t kStun = 14;
    // Running with a swing above: both the body and the upper body take
    // the kit's animation, unblended.
    auto d = ub::decide(data, running(), asked(kRun, true), slots(kRun, 0.0f, true, kAttack1H, 100.0f));
    ub::applyKitAnimation(slots(kRun, 0.0f, true, kAttack1H, 100.0f), d, kStun);
    CHECK(d.body.anim == static_cast<int32_t>(kStun));
    CHECK(d.bodyAction == ub::Action::Play);
    CHECK_FALSE(d.bodyBlend);
    CHECK(d.upper.anim == static_cast<int32_t>(kStun));
    CHECK(d.upperAction == ub::Action::Play);
    // Without an upper body of its own, only the body; what plays it goes on.
    d = ub::decide(data, ub::Unit{}, asked(kStand, true), slots(kStun));
    ub::applyKitAnimation(slots(kStun), d, kStun);
    CHECK(d.bodyAction == ub::Action::Keep);
    CHECK(d.upperAction == ub::Action::Keep);
}

TEST_CASE("a rider's state kit animation goes to its upper body (0x0071e400)", "[upper_body]") {
    const auto data = table();
    ub::Unit rider;
    rider.riding = true;
    // An upper-body animation the mount does not play: on the upper body,
    // the body left in its pose.
    auto d = ub::decide(data, rider, asked(kStand, true), slots(kMount));
    const auto bodyBefore = d.body;
    ub::applyRiderKitAnimation(data, slots(kMount), d, kChannelCast);
    CHECK(d.upper.anim == static_cast<int32_t>(kChannelCast));
    CHECK(d.upperAction == ub::Action::Play);
    CHECK(d.body.anim == bodyBefore.anim);
    // One the mount plays is the mount's alone.
    d = ub::decide(data, rider, asked(kStand, true), slots(kMount));
    ub::applyRiderKitAnimation(data, slots(kMount), d, kRun);
    CHECK(d.upper.anim == -1);
}
