// A dynamic object's model as the client makes it
// (rendering/dynamic_object_visual.hpp): which effect, how large, and its
// sequences.
#include <catch_amalgamated.hpp>

#include "rendering/dynamic_object_visual.hpp"

namespace dyn = wowee::rendering::dynamic_object;

TEST_CASE("the kit's WorldEffect, else its BaseEffect (0x00704b30)", "[dynamic_object]") {
    CHECK(dyn::effectRow(81, true, 82, true) == 81);
    // A WorldEffect the table lacks gives way to the BaseEffect.
    CHECK(dyn::effectRow(81, false, 82, true) == 82);
    CHECK(dyn::effectRow(0, false, 82, true) == 82);
    CHECK(dyn::effectRow(81, false, 82, false) == 0);
    CHECK(dyn::effectRow(0, false, 0, false) == 0);
}

TEST_CASE("the model's scale (0x00704d90)", "[dynamic_object]") {
    // Not an area spell: the radius over the model's vertex radius, times
    // the effect's Scale.
    CHECK(dyn::modelScale(0, 8.0f, 2.0f, true, 5.0f, 1.5f) == 6.0f);
    // A model without a radius: over the effect's AreaEffectSize.
    CHECK(dyn::modelScale(0, 8.0f, 0.0f, true, 4.0f, 1.0f) == 2.0f);
    // Neither: the Scale alone.
    CHECK(dyn::modelScale(0, 8.0f, 0.0f, true, 0.0f, 3.0f) == 3.0f);
    // No effect row and no model radius: 1, its Scale not asked.
    CHECK(dyn::modelScale(0, 8.0f, 0.0005f, false, 4.0f, 3.0f) == 1.0f);
    CHECK(dyn::modelScale(0, 8.0f, 4.0f, false, 0.0f, 0.0f) == 2.0f);
    // An area spell (1) and a far sight (2) keep the effect's own size.
    CHECK(dyn::modelScale(1, 8.0f, 2.0f, true, 5.0f, 1.5f) == 1.5f);
    CHECK(dyn::modelScale(2, 8.0f, 2.0f, true, 5.0f, 0.0f) == 1.0f);
    CHECK(dyn::sizedByRadius(3));
}

TEST_CASE("Birth, then Hold or Stand; Decay only when destroyed", "[dynamic_object]") {
    CHECK(dyn::firstSequence(true) == dyn::kAnimBirth);
    CHECK(dyn::firstSequence(false) == dyn::kAnimStand);
    CHECK(dyn::idleSequence(true) == dyn::kAnimHold);
    CHECK(dyn::idleSequence(false) == dyn::kAnimStand);
    CHECK(dyn::decaysOnRemoval(true, true));
    CHECK_FALSE(dyn::decaysOnRemoval(false, true));  // out of range: at once
    CHECK_FALSE(dyn::decaysOnRemoval(true, false));
}
