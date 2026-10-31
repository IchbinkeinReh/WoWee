// M2AnimationTrack::SequenceTable: the keys by sequence, a record only where
// a sequence has keys.
#include <catch_amalgamated.hpp>

#include <cmath>
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

// PackedQuat: a rotation key kept as the file has it.
using wowee::pipeline::PackedQuat;

namespace {
// The loader's decoding before keys were kept packed, verbatim.
glm::quat decodeAsBefore(int16_t x, int16_t y, int16_t z, int16_t w) {
    float fx = (x < 0) ? (x + 32768) / 32767.0f : (x - 32767) / 32767.0f;
    float fy = (y < 0) ? (y + 32768) / 32767.0f : (y - 32767) / 32767.0f;
    float fz = (z < 0) ? (z + 32768) / 32767.0f : (z - 32767) / 32767.0f;
    float fw = (w < 0) ? (w + 32768) / 32767.0f : (w - 32767) / 32767.0f;
    glm::quat q(fw, fx, fy, fz);
    float len = glm::length(q);
    if (len > 0.001f) q = q / len;
    else q = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return q;
}
}  // namespace

TEST_CASE("A packed key reads as the same floats the loader used to make", "[m2_sequence_table]") {
    const int16_t samples[][4] = {
        {-1, 32767, 32767, -1}, {12345, -20000, 32766, -32768}, {0, -32768, 100, -100},
        {32767, 32767, 32767, 32767},  // all zero: identity
        {-5000, 7000, -9000, 11000},
    };
    for (const auto& s : samples) {
        const glm::quat before = decodeAsBefore(s[0], s[1], s[2], s[3]);
        const glm::quat now = PackedQuat::fromDisk(s[0], s[1], s[2], s[3]);
        CHECK(now.x == before.x);
        CHECK(now.y == before.y);
        CHECK(now.z == before.z);
        CHECK(now.w == before.w);
    }
}

TEST_CASE("A packed key made from a quat rounds it to the encoding's steps", "[m2_sequence_table]") {
    const glm::quat id = PackedQuat{};
    CHECK(id.w == 1.0f);
    CHECK(id.x == 0.0f);
    const glm::quat q = glm::normalize(glm::quat(0.3f, -0.5f, 0.7f, -0.1f));
    const glm::quat back = PackedQuat(q);
    CHECK(std::abs(back.w - q.w) < 1e-4f);
    CHECK(std::abs(back.x - q.x) < 1e-4f);
    CHECK(std::abs(back.y - q.y) < 1e-4f);
    CHECK(std::abs(back.z - q.z) < 1e-4f);
    const glm::quat one = PackedQuat(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    CHECK(one.w == 1.0f);
    CHECK(one.x == 0.0f);
}
