// M2AnimationTrack::SequenceTable: the keys by sequence, a record only where
// a sequence has keys.
#include <catch_amalgamated.hpp>

#include <utility>

#include "pipeline/m2_loader.hpp"

using wowee::pipeline::M2AnimationTrack;

TEST_CASE("An absent sequence reads as empty and costs no record", "[m2_sequence_table]") {
    M2AnimationTrack t;
    t.sequences.resize(100);
    CHECK(t.sequences.size() == 100);
    CHECK(std::as_const(t.sequences)[42].timestamps.empty());
    CHECK(t.sequences.records().empty());
}

TEST_CASE("Writing a sequence makes its record, and only its", "[m2_sequence_table]") {
    M2AnimationTrack t;
    t.sequences.resize(10);
    t.sequences[3].timestamps = {0, 100};
    t.sequences[3].floatValues = {1.0f, 2.0f};
    t.sequences[7].timestamps = {5};
    t.sequences[7].floatValues = {3.0f};
    CHECK(t.sequences.records().size() == 2);
    const auto& c = t.sequences;
    CHECK(c[3].floatValues[1] == 2.0f);
    CHECK(c[7].timestamps[0] == 5u);
    CHECK(c[5].timestamps.empty());
}

TEST_CASE("dropEmpty takes back records left empty and keeps the others in place", "[m2_sequence_table]") {
    M2AnimationTrack t;
    t.sequences.resize(6);
    t.sequences[0].timestamps = {1};
    (void)t.sequences[1];  // touched for writing, never filled
    t.sequences[2].timestamps = {2};
    (void)t.sequences[4];
    t.sequences[5].timestamps = {5};
    CHECK(t.sequences.records().size() == 5);
    t.sequences.dropEmpty();
    CHECK(t.sequences.records().size() == 3);
    const auto& c = t.sequences;
    CHECK(c[0].timestamps[0] == 1u);
    CHECK(c[1].timestamps.empty());
    CHECK(c[2].timestamps[0] == 2u);
    CHECK(c[5].timestamps[0] == 5u);
}

TEST_CASE("Iteration visits every sequence in order", "[m2_sequence_table]") {
    M2AnimationTrack t;
    t.sequences.resize(4);
    t.sequences[2].timestamps = {9};
    int n = 0;
    int withKeys = -1;
    for (const auto& s : t.sequences) {
        if (!s.timestamps.empty()) withKeys = n;
        ++n;
    }
    CHECK(n == 4);
    CHECK(withKeys == 2);
}

TEST_CASE("Shrinking drops the records of the sequences cut off", "[m2_sequence_table]") {
    M2AnimationTrack t;
    t.sequences.resize(5);
    t.sequences[1].timestamps = {1};
    t.sequences[4].timestamps = {4};
    t.sequences.resize(3);
    CHECK(t.sequences.size() == 3);
    CHECK(t.sequences.records().size() == 1);
    CHECK(std::as_const(t.sequences)[1].timestamps[0] == 1u);
    t.sequences.push_back({});
    CHECK(t.sequences.size() == 4);
    CHECK(t.sequences.records().size() == 1);
}
