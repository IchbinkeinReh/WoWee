// MCAL alpha maps, and the row and column at the end of one that are not data.
//
// A chunk's alpha map is stored 64 texels wide and only 63 of them are
// painted: the file's last row and column carry whatever was left in them, and
// the client fills them from the row and column before. Left alone they are a
// strip of something else along two of a chunk's four sides, which puts a hard
// line between every pair of chunks - the ground textures not lining up, in a
// grid across the whole world.
//
// The chunks here are built by hand, so this needs no ADT, no device and no
// world.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <vector>

#include "pipeline/adt_alpha.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"

using wowee::pipeline::ALPHA_MAP_DIM;
using wowee::pipeline::ALPHA_MAP_PACKED;
using wowee::pipeline::ALPHA_MAP_SIZE;
using wowee::pipeline::decodeLayerAlpha;
using wowee::pipeline::MapChunk;

namespace {

/// A chunk with one base layer and one alpha layer, whose four-bit map is
/// filled by `nibbleAt(x, y)`.
template <typename F>
MapChunk packedChunk(F nibbleAt) {
    MapChunk chunk;
    chunk.layers.resize(2);
    // Layer 0 is the base and never has a map; layer 1's begins at zero.
    chunk.layers[1].flags = 0x100;   // use_alpha_map
    chunk.layers[1].offsetMCAL = 0;

    chunk.alphaMap.assign(ALPHA_MAP_PACKED, 0);
    for (size_t y = 0; y < ALPHA_MAP_DIM; ++y) {
        for (size_t x = 0; x < ALPHA_MAP_DIM; x += 2) {
            const uint8_t lo = static_cast<uint8_t>(nibbleAt(x, y) & 0x0F);
            const uint8_t hi = static_cast<uint8_t>(nibbleAt(x + 1, y) & 0x0F);
            chunk.alphaMap[(y * ALPHA_MAP_DIM + x) / 2] =
                static_cast<uint8_t>(lo | (hi << 4));
        }
    }
    return chunk;
}

uint8_t at(const std::vector<uint8_t>& alpha, size_t x, size_t y) {
    return alpha[y * ALPHA_MAP_DIM + x];
}

}  // namespace

TEST_CASE("a four-bit alpha map unpacks low nibble first", "[adt][alpha]") {
    // Every texel its own column number, so an unpack that swapped the two
    // halves of a byte would show immediately.
    const MapChunk chunk = packedChunk([](size_t x, size_t) { return x % 16; });
    std::vector<uint8_t> alpha;
    REQUIRE(decodeLayerAlpha(chunk, 1, alpha, 0));
    REQUIRE(alpha.size() == ALPHA_MAP_SIZE);
    CHECK(at(alpha, 0, 0) == 0);
    CHECK(at(alpha, 1, 0) == 17);
    CHECK(at(alpha, 5, 3) == 5 * 17);
}

TEST_CASE("the last row and column are taken from the ones before them",
          "[adt][alpha]") {
    // 15 everywhere the file paints, and 0 in the row and column it does not -
    // which is the shape of the fault: a strip of nothing along two sides.
    const MapChunk chunk = packedChunk([](size_t x, size_t y) {
        const bool unpainted = (x == ALPHA_MAP_DIM - 1) || (y == ALPHA_MAP_DIM - 1);
        return unpainted ? 0 : 15;
    });
    std::vector<uint8_t> alpha;
    REQUIRE(decodeLayerAlpha(chunk, 1, alpha, 0));

    // The strip is gone: the edge reads as what the texel inside it reads.
    for (size_t i = 0; i < ALPHA_MAP_DIM; ++i) {
        INFO("column " << i);
        CHECK(at(alpha, i, ALPHA_MAP_DIM - 1) == at(alpha, i, ALPHA_MAP_DIM - 2));
    }
    for (size_t i = 0; i < ALPHA_MAP_DIM; ++i) {
        INFO("row " << i);
        CHECK(at(alpha, ALPHA_MAP_DIM - 1, i) == at(alpha, ALPHA_MAP_DIM - 2, i));
    }
    // And what it reads is the painted value, not the leftover.
    CHECK(at(alpha, ALPHA_MAP_DIM - 1, ALPHA_MAP_DIM - 1) == 15 * 17);
}

TEST_CASE("a layer with no map of its own is left as the caller asked",
          "[adt][alpha]") {
    MapChunk chunk;
    chunk.layers.resize(1);   // the base only
    std::vector<uint8_t> alpha;
    CHECK_FALSE(decodeLayerAlpha(chunk, 0, alpha, 255));
    REQUIRE(alpha.size() == ALPHA_MAP_SIZE);
    CHECK(at(alpha, 10, 10) == 255);
}

TEST_CASE("a chunk with flag 0x8000 keeps its last row and column", "[adt][alpha]") {
    // CMapChunk::CreateChunkLayerTex (0x007b9890) hands MCNK flag 0x8000 to
    // 0x007b8e20, which then unpacks all 64 rows and columns as stored.
    MapChunk chunk = packedChunk([](size_t x, size_t y) {
        const bool edge = (x == ALPHA_MAP_DIM - 1) || (y == ALPHA_MAP_DIM - 1);
        return edge ? 3 : 15;
    });
    chunk.flags = 0x8000;
    std::vector<uint8_t> alpha;
    REQUIRE(decodeLayerAlpha(chunk, 1, alpha, 0));
    CHECK(at(alpha, 10, ALPHA_MAP_DIM - 1) == 3 * 17);
    CHECK(at(alpha, ALPHA_MAP_DIM - 1, 10) == 3 * 17);
    CHECK(at(alpha, 10, 10) == 15 * 17);
}

TEST_CASE("the alpha map spans the chunk, the textures repeat once a quad", "[adt][alpha][terrain]") {
    namespace pl = wowee::pipeline;
    // 0x007d06b0: the alpha maps 0 to 1 over the chunk, the textures eight
    // times over it.
    CHECK(pl::terrainLayerUV(0.0f) == 0.0f);
    CHECK(pl::terrainLayerUV(8.0f) == 1.0f);
    CHECK(pl::terrainLayerUV(4.5f) == Catch::Approx(0.5625f));
    CHECK(pl::kTerrainTextureRepeatsPerChunk == 8.0f);
    // The CPU reads agree: the far edge is texel 63, the near one texel 0.
    CHECK(pl::alphaTexelIndex(1.0f, 0.0f) == ALPHA_MAP_DIM - 1);
    CHECK(pl::alphaTexelIndex(0.0f, 0.0f) == 0);
    CHECK(pl::alphaTexelIndex(0.5f, 0.0f) == 32);
}

TEST_CASE("a scrolling terrain layer moves as the client moves it", "[adt][terrain]") {
    namespace pl = wowee::pipeline;
    // No flag 0x40, no movement.
    CHECK(pl::terrainLayerAnimOffset(0x07, 10.0f) == glm::vec2(0.0f));
    // Direction 0 is (-1, 0): x falls, which moves v the other way; speed 0
    // divides by 64.
    const glm::vec2 a = pl::terrainLayerAnimOffset(0x40, 32.0f);
    CHECK(a.x == Catch::Approx(0.0f));
    CHECK(a.y == Catch::Approx(0.5f));
    // Direction 2 is (0, 1) and wraps at 64; speed 7 divides by 1.
    const glm::vec2 b = pl::terrainLayerAnimOffset(0x40 | (7u << 3) | 2u, 70.0f);
    CHECK(b.x == Catch::Approx(-6.0f));
    CHECK(b.y == Catch::Approx(0.0f));
}
