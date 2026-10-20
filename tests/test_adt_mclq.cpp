// Which of an MCLQ chunk's 8x8 liquid tiles the ADT loader keeps.
//
// A tile's byte holds the liquid in its low nibble, 0x0F meaning none, and
// flags above it: 0x40 fishable, 0x80 deep water (the tiles 0x007c8bf0 counts
// for fatigue). The client skips a tile only for the 0x0F nibble (0x0079c1a0,
// 0x007c8360). The loader used to drop 0x80 tiles as "hidden", which took out
// most of the open sea: off Sunstrider Isle the ocean ended a few hundred
// yards from the shore, where the shallow 0x41 tiles give way to 0xC1.
#include <catch_amalgamated.hpp>

#include "pipeline/adt_loader.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

using wowee::pipeline::ADTLoader;
using wowee::pipeline::ADTTerrain;

namespace {

constexpr uint32_t kMCNK = 0x4D434E4B;
constexpr size_t kMcnkHeaderBytes = 128;
constexpr size_t kMclqBytes = 720;  // heights, 9x9 vertices, 8x8 tiles
constexpr size_t kMclqTilesAt = 8 + 81 * 8;

void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, sizeof(v));
    out.insert(out.end(), b, b + 4);
}

void writeU32At(std::vector<uint8_t>& buf, size_t offset, uint32_t v) {
    std::memcpy(buf.data() + offset, &v, sizeof(v));
}

/// One ocean MCNK (flag 0x08) whose MCLQ tiles all read `tile`, with the
/// first tile set to `firstTile`.
ADTTerrain oceanChunk(uint8_t tile, uint8_t firstTile) {
    std::vector<uint8_t> payload(kMcnkHeaderBytes, 0);
    writeU32At(payload, 0x00, 0x08);                         // flags: ocean
    writeU32At(payload, 0x60, kMcnkHeaderBytes);              // ofsLiquid
    writeU32At(payload, 0x64, static_cast<uint32_t>(kMclqBytes));  // sizeLiquid

    std::vector<uint8_t> mclq(kMclqBytes, 0);
    std::memset(mclq.data() + kMclqTilesAt, tile, 64);
    mclq[kMclqTilesAt] = firstTile;
    payload.insert(payload.end(), mclq.begin(), mclq.end());

    std::vector<uint8_t> data;
    appendU32(data, kMCNK);
    appendU32(data, static_cast<uint32_t>(payload.size()));
    data.insert(data.end(), payload.begin(), payload.end());
    return ADTLoader::load(data);
}

}  // namespace

TEST_CASE("MCLQ deep-water tiles are drawn", "[adt][water]") {
    // Open sea: ocean (1) with the fishable and deep bits, 0xC1.
    const ADTTerrain deep = oceanChunk(0xC1, 0xC1);
    REQUIRE(deep.waterData[0].layers.size() == 1);
    const auto& layer = deep.waterData[0].layers[0];
    REQUIRE(layer.mask.size() == 8);
    for (uint8_t row : layer.mask) CHECK(row == 0xFF);
}

TEST_CASE("MCLQ tiles with the 0x0F nibble are not", "[adt][water]") {
    // 0x0F alone and with the deep bit set: both mean no liquid.
    for (const uint8_t none : {uint8_t{0x0F}, uint8_t{0x8F}}) {
        const ADTTerrain t = oceanChunk(0x41, none);
        REQUIRE(t.waterData[0].layers.size() == 1);
        const auto& mask = t.waterData[0].layers[0].mask;
        CHECK(mask[0] == 0xFE);
        for (size_t row = 1; row < mask.size(); ++row) CHECK(mask[row] == 0xFF);
    }

    // A chunk with no liquid tile at all brings no layer.
    const ADTTerrain dry = oceanChunk(0x0F, 0x0F);
    CHECK(dry.waterData[0].layers.empty());
}
