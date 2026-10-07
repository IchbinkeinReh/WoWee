// Where the client puts ground clutter on a chunk (0x007d3390): its random
// generator (0x004c1510 / 0x00464580), the doodad wheel, the cell masks, the
// slope limit, and the per-texture buffer limits of 0x007b31e0
// (pipeline/detail_doodad_placement.hpp).

#include <catch_amalgamated.hpp>

#include "pipeline/detail_doodad_placement.hpp"

#include <cstring>
#include <map>
#include <vector>

using namespace wowee::pipeline;

namespace {
constexpr float kUnit = 33.333333f / 8.0f;

MapChunk flatChunk(uint32_t effectId) {
    MapChunk c{};
    c.heightMap.heights.fill(0.0f);
    c.heightMap.loaded = true;
    TextureLayer layer{};
    layer.effectId = effectId;
    c.layers.push_back(layer);
    return c;
}

struct Effects {
    std::map<uint32_t, GroundEffectRecord> rows;
    const GroundEffectRecord* operator()(uint32_t id) const {
        auto it = rows.find(id);
        return it == rows.end() ? nullptr : &it->second;
    }
};
}  // namespace

TEST_CASE("the client's random generator", "[clutter]") {
    // Against the table at 0x009f1700 stepped as 0x00464580 does.
    ClientRandom r((5u << 16) | 7u);
    CHECK(r.next() == 0x6ca97718u);
    CHECK(r.next() == 0xc8d777c7u);
    CHECK(r.next() == 0x4f0572d7u);
    CHECK(r.next() == 0x5d10a311u);
    ClientRandom q(0x2030309u);
    CHECK(q.next() == 0xa12df991u);
    CHECK(q.next() == 0x4311f478u);
    for (int i = 0; i < 1000; ++i) {
        const float f = q.nextSigned();
        CHECK(f >= -1.0f);
        CHECK(f <= 1.0f);
    }
}

TEST_CASE("the doodad wheel follows the weights thirteen slots apart", "[clutter]") {
    GroundEffectRecord r;
    r.doodadIds = {11, 22, 0, 0};
    r.weights = {3, 1, 0, 0};
    const auto w = groundEffectWheel(r);
    // 11 at 0, 13, 26&15=10; 22 at 39&15=7; the rest from ids[4..15 & 3].
    CHECK(w[0] == 11);
    CHECK(w[13] == 11);
    CHECK(w[10] == 11);
    CHECK(w[7] == 22);
    // Total 4: slot 52&15=4 takes ids[0], 65&15=1 ids[1], 78&15=14 ids[2]=0.
    CHECK(w[4] == 11);
    CHECK(w[1] == 22);
    CHECK(w[14] == 0);
}

TEST_CASE("density setting maps onto the client's cell picks", "[clutter]") {
    CHECK(detailCellPicks(0.0f) == 0);
    CHECK(detailCellPicks(0.1f) == 16);    // the cvar's floor
    CHECK(detailCellPicks(1.5f) == 64);    // the Blizzard slider's top
    CHECK(detailCellPicks(0.7f) == 30);
}

TEST_CASE("placement is the chunk's own and repeatable", "[clutter]") {
    Effects fx;
    GroundEffectRecord rec;
    rec.doodadIds = {7, 0, 0, 0};
    rec.weights = {16, 0, 0, 0};
    rec.density = 4;
    fx.rows[100] = rec;
    const MapChunk c = flatChunk(100);

    const auto a = placeDetailDoodads(c, 100, 200, 16, fx, kUnit);
    const auto b = placeDetailDoodads(c, 100, 200, 16, fx, kUnit);
    REQUIRE(a.size() == 16 * 4);  // every try lands on flat open ground
    REQUIRE(b.size() == a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].fracX == b[i].fracX);
        CHECK(a[i].doodadId == 7);
        CHECK(a[i].fracX >= 0.0f);
        CHECK(a[i].fracX <= 8.0f);
        CHECK(a[i].rotation >= 0.0f);
        CHECK(a[i].rotation <= 6.2832f);
        CHECK(a[i].scale >= 0.67f);
        CHECK(a[i].scale <= 1.33f);
    }
    const auto other = placeDetailDoodads(c, 101, 200, 16, fx, kUnit);
    CHECK(other[0].fracX != a[0].fracX);

    // Density 0 reads as 8 tries a cell.
    fx.rows[100].density = 0;
    CHECK(placeDetailDoodads(c, 100, 200, 16, fx, kUnit).size() == 16 * 8);
}

TEST_CASE("masked cells, holes and unknown effects grow nothing", "[clutter]") {
    Effects fx;
    GroundEffectRecord rec;
    rec.doodadIds = {7, 0, 0, 0};
    rec.weights = {16, 0, 0, 0};
    rec.density = 2;
    fx.rows[100] = rec;
    MapChunk c = flatChunk(100);
    c.noEffectDoodad = ~0ull;
    CHECK(placeDetailDoodads(c, 1, 2, 32, fx, kUnit).empty());
    c.noEffectDoodad = 0;
    c.holes = 0xFFFF;
    CHECK(placeDetailDoodads(c, 1, 2, 32, fx, kUnit).empty());
    c.holes = 0;
    c.layers[0].effectId = 555;
    CHECK(placeDetailDoodads(c, 1, 2, 32, fx, kUnit).empty());
    // The cell's 2-bit layer map points past the layers: nothing.
    c.layers[0].effectId = 100;
    c.doodadMapping.fill(0x55);
    CHECK(placeDetailDoodads(c, 1, 2, 32, fx, kUnit).empty());
}

TEST_CASE("steep ground grows nothing", "[clutter]") {
    Effects fx;
    GroundEffectRecord rec;
    rec.doodadIds = {7, 0, 0, 0};
    rec.weights = {16, 0, 0, 0};
    rec.density = 2;
    fx.rows[100] = rec;
    MapChunk c = flatChunk(100);
    // Three yards up per yard across: the face's up component is under 0.4.
    for (int y = 0; y <= 8; ++y)
        for (int x = 0; x <= 8; ++x)
            c.heightMap.heights[static_cast<size_t>(y * 17 + x)] = 3.0f * kUnit * static_cast<float>(x);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            c.heightMap.heights[static_cast<size_t>(9 + y * 17 + x)] = 3.0f * kUnit * (static_cast<float>(x) + 0.5f);
    CHECK(placeDetailDoodads(c, 1, 2, 32, fx, kUnit).empty());
}

TEST_CASE("a chunk's buffers hold four textures under the vertex limit", "[clutter]") {
    std::vector<DetailDoodadPlacement> ps;
    for (uint32_t i = 0; i < 6; ++i) ps.push_back({.doodadId = i + 1});
    std::map<uint32_t, DetailModelSize> sizes;
    for (uint32_t i = 1; i <= 6; ++i) sizes[i] = {.texture = "t" + std::to_string(i), .vertices = 8, .indices = 12};
    const auto lookup = [&](uint32_t id) -> const DetailModelSize* {
        auto it = sizes.find(id);
        return it == sizes.end() ? nullptr : &it->second;
    };
    const auto keep = applyDetailBatchLimits(ps, 16, lookup);
    CHECK(keep == std::vector<bool>{true, true, true, true, false, false});

    // One texture: 16 x 64 = 1024 vertices and as many indices, a total
    // kept strictly under. 12 indices a model is the tighter: 85 a buffer,
    // then the next three buffers of the same texture take as many again.
    std::vector<DetailDoodadPlacement> same(200, {.doodadId = 1});
    size_t n = 0;
    for (bool k : applyDetailBatchLimits(same, 16, lookup)) n += k ? 1 : 0;
    CHECK(n == 200);
    std::vector<DetailDoodadPlacement> many(1000, {.doodadId = 1});
    size_t m = 0;
    for (bool k : applyDetailBatchLimits(many, 16, lookup)) m += k ? 1 : 0;
    CHECK(m == 4 * 85);
}

TEST_CASE("detail doodad buffer counts come from the model's skin") {
    // 0x007b31e0 reads the skin's +0x4 (vertices) and +0xc (indices).
    std::vector<uint8_t> skin(48, 0);
    std::memcpy(skin.data(), "SKIN", 4);
    const uint32_t nv = 37, ni = 120;
    std::memcpy(skin.data() + 4, &nv, 4);
    std::memcpy(skin.data() + 12, &ni, 4);
    uint32_t v = 0, i = 0;
    REQUIRE(wowee::pipeline::detailSkinCounts(skin, v, i));
    CHECK(v == 37u);
    CHECK(i == 120u);
    skin[0] = 'X';
    CHECK_FALSE(wowee::pipeline::detailSkinCounts(skin, v, i));
    CHECK_FALSE(wowee::pipeline::detailSkinCounts(std::vector<uint8_t>(8, 0), v, i));
}
