// The ways one image of the interface is uploaded (ui/texture_variant.hpp).
#include <catch_amalgamated.hpp>

#include "ui/texture_variant.hpp"

using namespace wowee::ui;

TEST_CASE("a texture that repeats is an upload of its own", "[texture_variant]") {
    Widget w;
    CHECK(textureVariantOf(w) == 0);
    CHECK(textureKey("a", textureVariantOf(w)) == "a");
    // Sized by its image, and still not repeated (0x0048cb60).
    w.horizTile = w.vertTile = true;
    CHECK(textureVariantOf(w) == 0);
    w.repeatAcross = true;
    CHECK(textureVariantOf(w) == kVariantRepeatAcross);
    CHECK(textureKey("a", textureVariantOf(w)) == "a|across");
    w.repeatDown = true;
    CHECK(textureKey("a", textureVariantOf(w)) == "a|across|down");
    w.repeatAcross = false;
    CHECK(textureKey("a", textureVariantOf(w)) == "a|down");
    w.desaturated = w.emblemMask = true;
    CHECK(textureKey("a", textureVariantOf(w)) == "a|desat|mask|down");
}

// 0x004a2e00, 0x004a29d0
TEST_CASE("a backdrop's background is tiled by its tile size", "[texture_variant]") {
    Widget w;
    w.edgeSize = 16.0f;
    w.backdropTileSize = 40.0f;
    CHECK(backdropVariantOf(w) == 0);
    CHECK(backdropRepeats(w, 200.0f) == 1.0f);
    w.tileBackground = true;
    CHECK(backdropVariantOf(w) == kVariantRepeatAcross);
    CHECK(backdropRepeats(w, 200.0f) == 5.0f);
    CHECK(backdropRepeats(w, 120.0f) == 3.0f);
    w.backdropTileSize = 0.0f;
    CHECK(backdropRepeats(w, 200.0f) == 12.5f);
    w.edgeSize = 0.0f;
    CHECK(backdropRepeats(w, 200.0f) == 1.0f);
}
