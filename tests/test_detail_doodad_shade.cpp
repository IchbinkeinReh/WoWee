// What ground cover is lit by, as the client places it (0x007d3390): the
// terrain's MCCV doubled, its MCSH shadow and the face normal under it
// (pipeline/detail_doodad_shade.hpp).

#include <catch_amalgamated.hpp>

#include "pipeline/detail_doodad_shade.hpp"

#include <cmath>

using wowee::pipeline::MapChunk;
using wowee::pipeline::detailDoodadShade;

namespace {
constexpr float kUnit = 33.333333f / 8.0f;

MapChunk chunkWhere(float slopePerColumn) {
    MapChunk c{};
    c.heightMap.heights.fill(0.0f);
    c.heightMap.loaded = true;
    for (int y = 0; y <= 8; ++y)
        for (int x = 0; x <= 8; ++x)
            c.heightMap.heights[static_cast<size_t>(y * 17 + x)] = slopePerColumn * static_cast<float>(x);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            c.heightMap.heights[static_cast<size_t>(9 + y * 17 + x)] = slopePerColumn * (static_cast<float>(x) + 0.5f);
    return c;
}
}  // namespace

TEST_CASE("ground cover takes the terrain's colour doubled", "[clutter]") {
    MapChunk c = chunkWhere(0.0f);
    for (size_t i = 0; i < 145; ++i) {
        c.vertexShading[i * 4 + 0] = 0x40;  // B
        c.vertexShading[i * 4 + 1] = 0x7f;  // G
        c.vertexShading[i * 4 + 2] = 0xc0;  // R
        c.vertexShading[i * 4 + 3] = 0xff;
    }
    c.hasVertexShading = true;
    const auto s = detailDoodadShade(c, 3.3f, 4.7f, kUnit, true);
    CHECK(s.color.r == 1.0f);  // 0xc0 doubled, capped
    CHECK(s.color.g == Catch::Approx(254.0f / 255.0f));
    CHECK(s.color.b == Catch::Approx(128.0f / 255.0f));
    // GroundEffectDoodad flag 0x2: white.
    CHECK(detailDoodadShade(c, 3.3f, 4.7f, kUnit, false).color == glm::vec3(1.0f));
    // No MCCV: white.
    c.hasVertexShading = false;
    CHECK(detailDoodadShade(c, 3.3f, 4.7f, kUnit, true).color == glm::vec3(1.0f));
}

TEST_CASE("ground cover in the baked shadow has alpha 0", "[clutter]") {
    MapChunk c = chunkWhere(0.0f);
    c.shadowMap.assign(64 * 64, 0);
    // 1.92 texels a yard: the middle of texel (20, 10).
    c.shadowMap[10 * 64 + 20] = 1;
    const float fx = 20.5f / (1.92f * kUnit);
    const float fy = 10.5f / (1.92f * kUnit);
    CHECK(detailDoodadShade(c, fx, fy, kUnit, true).lit == 0.0f);
    CHECK(detailDoodadShade(c, fy, fx, kUnit, true).lit == 1.0f);
    // No MCSH: lit.
    c.shadowMap.clear();
    CHECK(detailDoodadShade(c, fx, fy, kUnit, true).lit == 1.0f);
}

TEST_CASE("ground cover is lit by the face normal under it", "[clutter]") {
    const auto flat = detailDoodadShade(chunkWhere(0.0f), 4.2f, 2.9f, kUnit, true);
    CHECK(flat.normal.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(flat.normal.y == Catch::Approx(0.0f).margin(1e-6));
    CHECK(flat.normal.z == Catch::Approx(1.0f));
    // Rising one a column: render y runs against the columns, so the face
    // leans toward +y by 1 in kUnit.
    for (float fx : {1.2f, 1.8f}) {
        for (float fy : {5.1f, 5.9f}) {
            const auto s = detailDoodadShade(chunkWhere(1.0f), fx, fy, kUnit, true);
            const glm::vec3 want = glm::normalize(glm::vec3(0.0f, 1.0f / kUnit, 1.0f));
            CHECK(s.normal.x == Catch::Approx(want.x).margin(1e-5));
            CHECK(s.normal.y == Catch::Approx(want.y).margin(1e-5));
            CHECK(s.normal.z == Catch::Approx(want.z).margin(1e-5));
        }
    }
}
