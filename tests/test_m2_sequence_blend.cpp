// Blending from one M2 sequence into the next.
//
// The 3.3.5a client keeps the sequence a model is leaving and weighs its pose
// by smoothstep(remaining / blendTime) of the NEW sequence's blendTime
// (FUN_00826c40 starts it, FUN_0082f0f0 weighs it each frame), and every
// sampler then mixes - slerps, for a rotation - from the new value toward the
// old. Without it a creature going from standing to running snapped between
// the two poses in a single frame.
//
// The oracle is the client's arithmetic as decompiled, and hand-built tracks
// whose blended values can be worked out on paper.
#include <catch_amalgamated.hpp>

#include <vector>

#include "rendering/m2_track_sampler.hpp"

using namespace wowee::rendering::m2_track;
using wowee::pipeline::M2AnimationTrack;
using wowee::pipeline::M2Bone;
using wowee::pipeline::M2Sequence;

namespace {

M2Sequence sequence(uint32_t duration, uint32_t blendTime) {
    M2Sequence s{};
    s.duration = duration;
    s.blendTime = blendTime;
    return s;
}

// A linear translation track holding `value` through each sequence.
M2AnimationTrack constantVec3(const std::vector<glm::vec3>& perSequence) {
    M2AnimationTrack t;
    t.interpolationType = 1;
    for (const auto& v : perSequence) {
        M2AnimationTrack::SequenceKeys keys;
        keys.timestamps = {0, 1000};
        keys.vec3Values = {v, v};
        t.sequences.push_back(keys);
    }
    return t;
}

} // namespace

TEST_CASE("Blend weight is a smoothstep of the time left", "[m2][blend]") {
    // One at the switch, a half halfway, none at or past the end.
    CHECK(sequenceBlendWeight(150.0f, 150.0f) == Catch::Approx(1.0f));
    CHECK(sequenceBlendWeight(75.0f, 150.0f) == Catch::Approx(0.5f));
    CHECK(sequenceBlendWeight(37.5f, 150.0f) == Catch::Approx(0.15625f));
    CHECK(sequenceBlendWeight(0.0f, 150.0f) == 0.0f);
    CHECK(sequenceBlendWeight(-10.0f, 150.0f) == 0.0f);
    // Under a millisecond left counts as over, as the client's integer clock has it.
    CHECK(sequenceBlendWeight(0.5f, 150.0f) == 0.0f);
    // A sequence with no blendTime snaps.
    CHECK(sequenceBlendWeight(100.0f, 0.0f) == 0.0f);
}

TEST_CASE("The old sequence keeps running through the blend", "[m2][blend]") {
    const std::vector<M2Sequence> seqs = {sequence(1000, 200), sequence(800, 150)};
    SequenceBlend blend;
    // Leave sequence 0 at 900 ms for sequence 1, at clock 5000.
    beginSequenceBlend(blend, 5000.0f, 0, 900.0f, true, false, 1, seqs[1].blendTime, seqs);
    REQUIRE(blend.fromSequence == 0);
    CHECK(blend.blendTimeMs == 150.0f);  // the new sequence's blendTime

    const BlendSample start = currentBlend(blend, 5000.0f, seqs, 1, 0.0f);
    CHECK(start.sequenceIndex == 0);
    CHECK(start.timeMs == Catch::Approx(900.0f));
    CHECK(start.weight == Catch::Approx(1.0f));

    // 125 ms on: the old sequence has wrapped past its 1000 ms end.
    const BlendSample later = currentBlend(blend, 5125.0f, seqs, 1, 125.0f);
    CHECK(later.timeMs == Catch::Approx(25.0f));
    CHECK(later.weight == Catch::Approx(sequenceBlendWeight(25.0f, 150.0f)));

    CHECK(currentBlend(blend, 5150.0f, seqs, 1, 150.0f).weight == 0.0f);
}

TEST_CASE("A held sequence holds its last frame while it blends out", "[m2][blend]") {
    const std::vector<M2Sequence> seqs = {sequence(1000, 200), sequence(800, 150)};
    SequenceBlend blend;
    beginSequenceBlend(blend, 0.0f, 0, 1000.0f, false, true, 1, 150, seqs);
    CHECK(currentBlend(blend, 100.0f, seqs, 1, 100.0f).timeMs == Catch::Approx(1000.0f));
}

TEST_CASE("Starting a sequence follows FUN_00826c40's rules", "[m2][blend]") {
    const std::vector<M2Sequence> seqs = {sequence(1000, 200), sequence(800, 200),
                                          sequence(600, 200)};

    SECTION("a blend whose old pose still weighs more than a half runs on") {
        SequenceBlend blend;
        beginSequenceBlend(blend, 0.0f, 0, 100.0f, true, false, 1, 200, seqs);
        beginSequenceBlend(blend, 50.0f, 1, 50.0f, true, false, 2, 200, seqs);
        CHECK(blend.fromSequence == 0);
        CHECK(blend.startClockMs == 0.0f);
    }
    SECTION("past the half the playing sequence becomes the old one") {
        SequenceBlend blend;
        beginSequenceBlend(blend, 0.0f, 0, 100.0f, true, false, 1, 200, seqs);
        beginSequenceBlend(blend, 150.0f, 1, 150.0f, true, false, 2, 200, seqs);
        CHECK(blend.fromSequence == 1);
        CHECK(blend.fromTimeMs == 150.0f);
        CHECK(blend.startClockMs == 150.0f);
    }
    SECTION("asking again for a sequence stopped at its end does not blend") {
        SequenceBlend blend;
        beginSequenceBlend(blend, 0.0f, 0, 1000.0f, false, true, 0, 200, seqs);
        CHECK(blend.fromSequence == -1);
    }
    SECTION("restarting one still playing blends from where it was") {
        SequenceBlend blend;
        beginSequenceBlend(blend, 0.0f, 0, 400.0f, true, false, 0, 200, seqs);
        CHECK(blend.fromSequence == 0);
        CHECK(currentBlend(blend, 0.0f, seqs, 0, 0.0f).weight == Catch::Approx(1.0f));
        // The same sequence at the same time is no blend at all.
        CHECK(currentBlend(blend, 0.0f, seqs, 0, 400.0f).weight == 0.0f);
    }
}

TEST_CASE("A bone mixes from the new pose toward the old", "[m2][blend]") {
    M2Bone bone{};
    bone.translation = constantVec3({glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f)});
    bone.scale = constantVec3({glm::vec3(1.0f), glm::vec3(3.0f)});
    bone.rotation.interpolationType = 1;
    const glm::quat quarterTurn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    for (const glm::quat& q : {glm::quat(1, 0, 0, 0), quarterTurn}) {
        M2AnimationTrack::SequenceKeys keys;
        keys.timestamps = {0, 1000};
        keys.quatValues = {q, q};
        bone.rotation.sequences.push_back(keys);
    }
    const std::vector<uint32_t> noGlobals;

    // Playing sequence 1, a quarter of the way back toward sequence 0.
    const BlendSample blend{.sequenceIndex = 0, .timeMs = 0.0f, .weight = 0.25f};
    const BoneTRS trs = sampleBone(bone, 1, 0.0f, 0.0f, noGlobals, blend);
    CHECK(trs.translation.x == Catch::Approx(7.5f));
    CHECK(trs.scale.y == Catch::Approx(2.5f));
    const float angle = glm::degrees(glm::angle(trs.rotation));
    CHECK(angle == Catch::Approx(67.5f).margin(0.01f));

    // No blend: the playing sequence alone.
    const BoneTRS plain = sampleBone(bone, 1, 0.0f, 0.0f, noGlobals);
    CHECK(plain.translation.x == Catch::Approx(10.0f));

    // A discrete track returns its own value before the client blends it.
    bone.translation.interpolationType = 0;
    CHECK(sampleBone(bone, 1, 0.0f, 0.0f, noGlobals, blend).translation.x ==
          Catch::Approx(10.0f));

    // So does a track on a global sequence.
    bone.scale.globalSequence = 0;
    const std::vector<uint32_t> oneGlobal = {1000};
    CHECK(sampleBone(bone, 1, 0.0f, 0.0f, oneGlobal, blend).scale.y == Catch::Approx(1.0f));
}
