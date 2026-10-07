// A cube map from a BLP, as the client's loader makes one
// (rendering/cube_texture.hpp): a six-wide strip, and the square each face
// is read from.

#include <catch_amalgamated.hpp>

#include "rendering/cube_texture.hpp"

using wowee::rendering::cubeStripSquare;
using wowee::rendering::isCubeStrip;

TEST_CASE("a BLP six times as wide as high is a cube (0x004b95b0)", "[cube]") {
    CHECK(isCubeStrip(768, 128));
    CHECK(isCubeStrip(6, 1));
    CHECK_FALSE(isCubeStrip(256, 256));
    CHECK_FALSE(isCubeStrip(512, 128));
    CHECK_FALSE(isCubeStrip(0, 0));
}

TEST_CASE("each device face reads the strip square 0x004b7aa0 names", "[cube]") {
    // +X, -X, +Y, -Y, +Z, -Z.
    CHECK(cubeStripSquare(0) == 0);
    CHECK(cubeStripSquare(1) == 2);
    CHECK(cubeStripSquare(2) == 4);
    CHECK(cubeStripSquare(3) == 5);
    CHECK(cubeStripSquare(4) == 3);
    CHECK(cubeStripSquare(5) == 1);
}
