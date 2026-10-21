// AnimationData's fallbacks: what a model plays for an animation it lacks
// (0x00826350) and what a unit is put in for one it is asked (0x007176f0).
#include <catch_amalgamated.hpp>

#include "rendering/animation/animation_data.hpp"

#include <initializer_list>
#include <set>

namespace a = wowee::rendering::anim;

namespace {

// 3.3.5's rows for the animations below (ID, Flags, Fallback, BehaviorID,
// BehaviorTier), the rest standing in as ground rows that fall back to
// Stand - the table is indexed by id, as the client's is.
a::AnimationDataTable table() {
    std::vector<a::AnimationDataRecord> rows;
    for (uint32_t id = 0; id < 506; ++id) rows.push_back({.id = id, .flags = 0, .fallback = 0, .behavior = id, .tier = 0});
    const auto set = [&rows](uint32_t id, uint32_t flags, uint32_t fallback, uint32_t behavior, uint32_t tier) {
        rows[id] = {.id = id, .flags = flags, .fallback = fallback, .behavior = behavior, .tier = tier};
    };
    set(0, 0x3, 147, 0, 0);     // Stand
    set(1, 0xa, 0, 1, 0);       // Death
    set(4, 0x6, 0, 4, 0);       // Walk
    set(5, 0x6, 0, 5, 0);       // Run
    set(6, 0x2b, 1, 6, 0);      // Dead
    set(13, 0x11, 4, 13, 0);    // Walkbackwards
    set(42, 0x6, 4, 42, 0);     // Swim
    set(45, 0x16, 42, 45, 0);   // SwimBackwards
    set(114, 0x0, 0, 114, 0);   // KneelStart
    set(115, 0x22, 114, 115, 0);  // KneelLoop
    set(116, 0x10, 114, 116, 0);  // KneelEnd
    set(146, 0x10, 148, 146, 0);  // Close
    set(147, 0x20, 146, 147, 0);  // Closed
    set(148, 0x13, 146, 148, 0);  // Open
    set(193, 0x2, 0, 193, 0);     // Hover
    set(229, 0x3, 376, 0, 3);     // FlyStand
    set(233, 0x6, 229, 4, 3);     // FlyWalk
    set(234, 0x6, 229, 5, 3);     // FlyRun
    set(375, 0x10, 377, 146, 3);  // FlyClose
    set(376, 0x20, 375, 147, 3);  // FlyClosed
    set(377, 0x10, 375, 148, 3);  // FlyOpen
    set(422, 0x2, 229, 193, 3);   // FlyHover
    return a::AnimationDataTable(std::move(rows));
}

struct Model {
    std::set<uint32_t> ids;
    bool operator()(uint32_t id) const { return ids.count(id) != 0; }
};

a::ModelAnimation shown(const a::AnimationDataTable& t, uint32_t id, std::initializer_list<uint32_t> ids) {
    Model m{ids};
    return a::modelAnimation(t, id, m, *m.ids.begin());
}

}  // namespace

TEST_CASE("BehaviorID, and 0x1fa for an id with no row (0x007176b0)", "[animation_data]") {
    const auto t = table();
    CHECK(t.behavior(5) == 5);
    CHECK(t.behavior(234) == 5);
    CHECK(t.behavior(229) == 0);
    CHECK(t.behavior(600) == a::kNoBehavior);
    CHECK(a::AnimationDataTable().behavior(0) == a::kNoBehavior);
}

TEST_CASE("a model plays its own sequence, or the first along the Fallbacks (0x00826350)", "[animation_data]") {
    const auto t = table();
    const auto own = shown(t, 5, {0, 5});
    CHECK(own.id == 5);
    CHECK(own.mode == a::PlayMode::Forward);
    // Run lacking: Stand, its fallback, forward.
    const auto run = shown(t, 5, {0, 4});
    CHECK(run.id == 0);
    CHECK(run.mode == a::PlayMode::Forward);
}

TEST_CASE("a Fallback through Flags 0x10 plays backwards, through 0x20 held", "[animation_data]") {
    const auto t = table();
    // Walkbackwards is Walk the other way round.
    const auto back = shown(t, 13, {0, 4});
    CHECK(back.id == 4);
    CHECK(back.mode == a::PlayMode::Reversed);
    // SwimBackwards (0x10) on to Swim and Walk: still the other way round.
    const auto swimBack = shown(t, 45, {0, 4});
    CHECK(swimBack.id == 4);
    CHECK(swimBack.mode == a::PlayMode::Reversed);
    // Dead is Death's last frame.
    const auto dead = shown(t, 6, {0, 1});
    CHECK(dead.id == 1);
    CHECK(dead.mode == a::PlayMode::HoldEnd);
    // KneelLoop the end of KneelStart, KneelEnd KneelStart backwards.
    CHECK(shown(t, 115, {0, 114}).mode == a::PlayMode::HoldEnd);
    CHECK(shown(t, 116, {0, 114}).mode == a::PlayMode::Reversed);
    // Held once, nothing turns it round after: Closed (0x20) holds before
    // Close's 0x10 can count, and Open plays on its last frame.
    const auto held = shown(t, 147, {148});
    CHECK(held.id == 148);
    CHECK(held.mode == a::PlayMode::HoldEnd);
}

TEST_CASE("a chain that reaches nothing ends on Stand, else Closed, else the first sequence",
          "[animation_data]") {
    const auto t = table();
    // 600 has no row.
    CHECK(shown(t, 600, {0, 50}).id == 0);
    // No Stand: Closed (0x93).
    CHECK(shown(t, 600, {50, 147}).id == 147);
    // Neither: the first sequence.
    Model m{{50, 60}};
    CHECK(a::modelAnimation(t, 600, m, 60).id == 60);
    // A row's chain goes on through Stand's own fallback, Closed.
    CHECK(shown(t, 300, {50, 147}).id == 147);
    // Without a table the id itself, else the stand-ins.
    CHECK(a::modelAnimation(a::AnimationDataTable(), 13, Model{{0, 4}}, 0).id == 0);
}

TEST_CASE("a model plays an animation it or a Fallback of it has (0x00826050)", "[animation_data]") {
    const auto t = table();
    CHECK(a::modelPlays(t, 13, Model{{4}}));
    CHECK_FALSE(a::modelPlays(t, 13, Model{{50}}));
    CHECK_FALSE(a::modelPlays(t, 600, Model{{600}}));
    // FlyRun on to FlyStand.
    CHECK(a::modelPlays(t, 234, Model{{229}}));
}

TEST_CASE("the unit's tier picks the animation of that behaviour (0x007176f0)", "[animation_data]") {
    const auto t = table();
    // On the ground: what it was asked.
    CHECK(a::unitAnimation(t, 5, 0, Model{{0, 5}}) == 5);
    CHECK(a::unitAnimation(t, 13, 0, Model{{0, 4}}) == 13);
    // Flying with flight animations: FlyRun for Run - played by the model
    // as FlyStand where it has no FlyRun.
    CHECK(a::unitAnimation(t, 5, 3, Model{{0, 5, 234}}) == 234);
    CHECK(a::unitAnimation(t, 5, 3, Model{{0, 5, 229}}) == 234);
    // Without them: the ground's, the tier's parent.
    CHECK(a::unitAnimation(t, 5, 3, Model{{0, 5}}) == 5);
    // Hover goes to Fly, then the ground.
    CHECK(a::unitAnimation(t, 193, 2, Model{{0, 422}}) == 422);
    CHECK(a::unitAnimation(t, 193, 2, Model{{0, 193}}) == 193);
    // A flight animation asked is itself.
    CHECK(a::unitAnimation(t, 234, 0, Model{{229}}) == 234);
    // Nothing along the way: the tier's stand where the model has it,
    // else the parent tier's, else as asked.
    CHECK(a::unitAnimation(t, 600, 0, Model{{0, 50}}) == 0);
    CHECK(a::unitAnimation(t, 600, 3, Model{{229, 50}}) == 229);
    CHECK(a::unitAnimation(t, 600, 3, Model{{0, 50}}) == 0);
    CHECK(a::unitAnimation(t, 600, 3, Model{{50}}) == 600);
    // No table: as asked.
    CHECK(a::unitAnimation(a::AnimationDataTable(), 5, 3, Model{{234}}) == 5);
}
