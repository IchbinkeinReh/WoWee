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

#include "rendering/animation/animation_ids.hpp"
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
