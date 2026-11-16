// A placed doodad whose model is emitters alone - no vertices, no skin - is
// kept and drawn, as the client draws the smoke and steam columns
// (BLACKSMITH_SMOKE, STEAM02-04, BUBBLESB_BIGEMITTER) over forges, ships and
// Borean Tundra's pools. A model with nothing at all is still dropped.
#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"

using wowee::pipeline::M2Model;

TEST_CASE("an emitter-only model is drawable, an empty one is not", "[m2][doodad]") {
    M2Model empty{};
    CHECK_FALSE(empty.isValid());
    CHECK_FALSE(empty.hasDrawableContent());

    M2Model smoke{};
    smoke.particleEmitters.resize(2);
    CHECK_FALSE(smoke.isValid());
    CHECK(smoke.hasDrawableContent());

    M2Model trail{};
    trail.ribbonEmitters.resize(1);
    CHECK(trail.hasDrawableContent());

    M2Model mesh{};
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    CHECK(mesh.isValid());
    CHECK(mesh.hasDrawableContent());
}
