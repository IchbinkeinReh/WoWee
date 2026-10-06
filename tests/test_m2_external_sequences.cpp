// Which of a model's sequences have their keyframes in an .anim file.
//
// Flag 0x20 says a sequence carries its data inside the M2. Every other one is
// empty until its file is read, and one that is skipped plays the bind pose:
// that is how other players stood still through their attacks, casts, emotes
// and jumps, when only Stand, Walk, Run and the sit states were asked for.
#include <catch_amalgamated.hpp>

#include <vector>

#include "pipeline/m2_loader.hpp"

using wowee::pipeline::M2Model;
using wowee::pipeline::M2Sequence;
using wowee::pipeline::externalSequenceIndices;

namespace {

M2Sequence sequence(uint32_t id, uint32_t flags) {
    M2Sequence s{};
    s.id = id;
    s.flags = flags;
    return s;
}

// Stand inside the model; Walk, an attack and a spell cast outside it - which
// is how a character model ships them.
M2Model characterLikeModel() {
    M2Model model;
    model.sequences = {sequence(0, 0x20), sequence(4, 0x00), sequence(17, 0x00),
                       sequence(0, 0x00), sequence(52, 0x01)};
    return model;
}

} // namespace

TEST_CASE("an empty list asks for every external sequence", "[m2][animation]") {
    const auto indices = externalSequenceIndices(characterLikeModel());
    CHECK(indices == std::vector<uint32_t>{1, 2, 3, 4});
}

TEST_CASE("a sequence inside the model is never asked for", "[m2][animation]") {
    // Stand's primary carries 0x20; its fidget variation at index 3 does not.
    const auto indices = externalSequenceIndices(characterLikeModel(), {0});
    CHECK(indices == std::vector<uint32_t>{3});
}

TEST_CASE("a filter keeps only the animations it names", "[m2][animation]") {
    const auto indices = externalSequenceIndices(characterLikeModel(), {4, 52, 999});
    CHECK(indices == std::vector<uint32_t>{1, 4});
}

TEST_CASE("a model with no sequences asks for nothing", "[m2][animation]") {
    CHECK(externalSequenceIndices(M2Model{}).empty());
}
