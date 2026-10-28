// What a spell visual kit played on a unit does to its animation: the
// unit's part of 0x0073b140.
#include <catch_amalgamated.hpp>

#include "rendering/animation/kit_animation.hpp"

namespace a = wowee::rendering::anim;
namespace ka = wowee::rendering::kit_animation;
using Kind = ka::Action::Kind;

namespace {

a::AnimationDataTable table() {
    std::vector<a::AnimationDataRecord> rows;
    for (uint32_t id = 0; id < 506; ++id) rows.push_back({.id = id, .flags = 0, .fallback = 0, .behavior = id, .tier = 0});
    return a::AnimationDataTable(std::move(rows));
}

constexpr int32_t kSpellPrecast = 31, kReadySpellDirected = 51, kSpellCastDirected = 53, kCombatWound = 9,
                  kCombatCritical = 10;

}  // namespace

TEST_CASE("a precast kit's start, then its loop with a cast time", "[kit_animation]") {
    const auto data = table();
    ka::KitPlay precast{.type = ka::kTypePrecast, .startAnim = kSpellPrecast, .anim = kReadySpellDirected,
                        .kitFlags = 0x8, .castTime = true};
    auto act = ka::onKitPlayed(data, precast, 0);
    CHECK(act.kind == Kind::Start);
    CHECK(act.anim == static_cast<uint32_t>(kSpellPrecast));
    CHECK(act.askFlags == 0x4u);
    // Without a StartAnimID, the AnimID, looped - only with a cast time,
    // and not over the cast kit's animation.
    precast.startAnim = -1;
    act = ka::onKitPlayed(data, precast, 0);
    CHECK(act.kind == Kind::Play);
    CHECK(act.anim == static_cast<uint32_t>(kReadySpellDirected));
    CHECK_FALSE(act.once);
    CHECK(act.clearsStart);
    precast.castKitAnim = kSpellCastDirected;
    CHECK(ka::onKitPlayed(data, precast, kSpellCastDirected).kind == Kind::None);
    precast.castKitAnim = -1;
    precast.castTime = false;
    act = ka::onKitPlayed(data, precast, 0);
    CHECK(act.kind == Kind::None);
    CHECK(act.clearsStart);
}

TEST_CASE("a cast or impact kit's animation, once; the wounds as wounds", "[kit_animation]") {
    const auto data = table();
    auto act = ka::onKitPlayed(data, {.type = 1, .startAnim = kSpellPrecast, .anim = kSpellCastDirected}, 0);
    CHECK(act.kind == Kind::Play);
    CHECK(act.anim == static_cast<uint32_t>(kSpellCastDirected));
    CHECK(act.once);
    CHECK_FALSE(act.holdsCastLoop);
    act = ka::onKitPlayed(data, {.type = 1, .anim = kCombatWound}, 0);
    CHECK(act.kind == Kind::Wound);
    CHECK_FALSE(act.critical);
    act = ka::onKitPlayed(data, {.type = 1, .anim = kCombatCritical}, 0);
    CHECK(act.critical);
    // A visual kit's (type 0) holds the cast loop while it plays.
    CHECK(ka::onKitPlayed(data, {.type = ka::kTypeVisual, .anim = kSpellCastDirected}, 0).holdsCastLoop);
    // None without an AnimID, and none for a state kit.
    CHECK(ka::onKitPlayed(data, {.type = 1}, 0).kind == Kind::None);
    CHECK(ka::onKitPlayed(data, {.type = ka::kTypeState, .anim = kSpellCastDirected}, 0).kind == Kind::None);
}
