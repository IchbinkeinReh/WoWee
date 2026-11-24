// Which variation of an animation plays, and what a unit's stand state plays.
//
// The 3.3.5a client rolls 0..0x7FFF and walks a sequence's variation chain,
// spending each sequence's frequency, to choose between Stand and its fidgets
// (FUN_00826e60); WoWee always played the primary, so creatures never
// fidgeted. A unit's stand state - byte 0 of UNIT_FIELD_BYTES_1 - picks the
// sit, sleep, chair or kneel loop and the transitions into and out of it.
//
// The oracle is the client's arithmetic as decompiled, and the 3.3.5
// AnimationData IDs.
#include <catch_amalgamated.hpp>

#include <vector>

#include "rendering/animation/animation_data.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/m2_shown_sequence.hpp"
#include "rendering/m2_track_sampler.hpp"

using wowee::pipeline::M2Sequence;
using wowee::rendering::m2_track::pickSequenceVariation;
namespace anim = wowee::rendering::anim;

namespace {

M2Sequence variation(int16_t frequency, int16_t next) {
    M2Sequence s{};
    s.frequency = frequency;
    s.nextAnimation = next;
    return s;
}

} // namespace

TEST_CASE("the roll walks the variation chain by frequency", "[m2][animation]") {
    // Stand at 0x2000, its fidget at 0x5FFF: together they cover the roll.
    const std::vector<M2Sequence> seqs = {variation(0x2000, 1), variation(0x5FFF, -1)};
    CHECK(pickSequenceVariation(seqs, 0, 0x0000) == 0);
    CHECK(pickSequenceVariation(seqs, 0, 0x1FFF) == 0);
    CHECK(pickSequenceVariation(seqs, 0, 0x2000) == 1);
    CHECK(pickSequenceVariation(seqs, 0, 0x7FFE) == 1);
}

TEST_CASE("a roll past the chain plays the primary", "[m2][animation]") {
    const std::vector<M2Sequence> seqs = {variation(0x1000, 1), variation(0x1000, -1)};
    CHECK(pickSequenceVariation(seqs, 0, 0x7FFF) == 0);
}

TEST_CASE("a chain that loops back on itself still ends", "[m2][animation]") {
    const std::vector<M2Sequence> seqs = {variation(1, 1), variation(1, 0)};
    CHECK(pickSequenceVariation(seqs, 0, 0x7FFF) == 0);
}

TEST_CASE("an out-of-range primary is passed through", "[m2][animation]") {
    const std::vector<M2Sequence> seqs = {variation(0x7FFF, -1)};
    CHECK(pickSequenceVariation(seqs, -1, 0) == -1);
    CHECK(pickSequenceVariation(seqs, 5, 0) == 5);
}

TEST_CASE("each stand state plays its loop and transitions", "[animation]") {
    STATIC_CHECK(anim::standStateAnims(0).loop == anim::STAND);
    STATIC_CHECK(anim::standStateAnims(1).loop == anim::SITTING);
    STATIC_CHECK(anim::standStateAnims(1).down == anim::SIT_GROUND_DOWN);
    STATIC_CHECK(anim::standStateAnims(1).up == anim::SIT_GROUND_UP);
    STATIC_CHECK(anim::standStateAnims(3).loop == anim::SLEEP);
    STATIC_CHECK(anim::standStateAnims(4).loop == anim::SIT_CHAIR_LOW);
    STATIC_CHECK(anim::standStateAnims(6).loop == anim::SIT_CHAIR_HIGH);
    STATIC_CHECK(anim::standStateAnims(7).loop == anim::DEAD);
    STATIC_CHECK(anim::standStateAnims(8).loop == anim::KNEEL_LOOP);
}

namespace {

// AnimationData's rows for Stand and the game objects' own.
anim::AnimationDataTable doorRows() {
    std::vector<anim::AnimationDataRecord> rows;
    for (uint32_t i = 0; i < 160; ++i) rows.push_back({.id = i, .behavior = i});
    const auto set = [&](uint32_t i, uint32_t flags, uint32_t fallback) {
        rows[i].flags = flags;
        rows[i].fallback = fallback;
    };
    set(0, 3, 147);
    set(146, 16, 148);
    set(147, 32, 146);
    set(148, 19, 146);
    set(149, 35, 148);
    return anim::AnimationDataTable(std::move(rows));
}

std::vector<M2Sequence> sequencesOf(std::initializer_list<uint16_t> ids) {
    std::vector<M2Sequence> seqs;
    for (uint16_t id : ids) {
        M2Sequence s = variation(0x7FFF, -1);
        s.id = id;
        seqs.push_back(s);
    }
    return seqs;
}

}  // namespace

// G_Crate01.m2 and G_Barrel.m2 have Open first: as doodads they opened and shut for ever.
TEST_CASE("a model nobody asks anything of stands", "[m2][animation]") {
    namespace ms = wowee::rendering::m2_sequence;
    const auto rows = doorRows();
    auto crate = sequencesOf({148, 146, 149, 147, 0});
    CHECK(ms::idle(rows, crate, 0).index == 4);
    CHECK(ms::idle(rows, crate, 0).mode == anim::PlayMode::Forward);
    // Without Stand it is Closed, and without that what stands in for it.
    crate.pop_back();
    CHECK(ms::idle(rows, crate, 0).index == 3);
    crate.pop_back();
    CHECK(ms::idle(rows, crate, 0).index == 1);
    CHECK(ms::idle(rows, crate, 0).mode == anim::PlayMode::HoldEnd);
    // A model with none of them plays its first.
    CHECK(ms::idle(rows, sequencesOf({4, 5}), 0).index == 0);
    CHECK(ms::idle(rows, {}, 0).index == -1);
    // Any variation of Stand.
    auto fidgets = sequencesOf({148, 0, 0});
    fidgets[1].frequency = 0x2000;
    fidgets[1].nextAnimation = 2;
    fidgets[2].frequency = 0x5FFF;
    CHECK(ms::idle(rows, fidgets, 0x1FFF).index == 1);
    CHECK(ms::idle(rows, fidgets, 0x2000).index == 2);

    M2Sequence once{};
    once.flags = 0x21;
    CHECK(ms::playsOnce(once));
    once.flags = 0x20;
    CHECK_FALSE(ms::playsOnce(once));
}
