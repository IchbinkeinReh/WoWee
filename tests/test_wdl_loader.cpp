// The .wdl horizon (0x007cc310, 0x007cc0b0, 0x007d5150, 0x007d5240).
#include <catch_amalgamated.hpp>

#include "pipeline/wdl_loader.hpp"

#include <cstring>

using namespace wowee::pipeline;

namespace {
void put32(std::vector<uint8_t>& d, uint32_t v) {
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putTag(std::vector<uint8_t>& d, const char* t) {
    for (int i = 3; i >= 0; --i) d.push_back(static_cast<uint8_t>(t[i]));
}
}  // namespace

TEST_CASE("a .wdl's tiles are read through MAOF, with MARE heights and MAHO holes", "[wdl]") {
    std::vector<uint8_t> d;
    putTag(d, "MVER"); put32(d, 4); put32(d, 18);
    putTag(d, "MWMO"); put32(d, 0);
    putTag(d, "MWID"); put32(d, 0);
    putTag(d, "MODF"); put32(d, 0);
    putTag(d, "MAOF"); put32(d, 64 * 64 * 4);
    const size_t maof = d.size();
    d.resize(d.size() + 64 * 64 * 4, 0);
    // One tile at row 30, column 33.
    const uint32_t at = static_cast<uint32_t>(d.size());
    std::memcpy(d.data() + maof + (30 * 64 + 33) * 4, &at, 4);
    putTag(d, "MARE"); put32(d, 1090);
    for (int k = 0; k < wdl::kVertices; ++k) {
        const int16_t h = static_cast<int16_t>(k == 5 ? -40 : (k == 400 ? 300 : 10));
        d.push_back(static_cast<uint8_t>(h & 0xff));
        d.push_back(static_cast<uint8_t>((h >> 8) & 0xff));
    }
    putTag(d, "MAHO"); put32(d, 32);
    for (int r = 0; r < 16; ++r) { d.push_back(r == 2 ? 0x05 : 0); d.push_back(0); }

    const WdlData w = parseWDL(d);
    REQUIRE(w.tiles.size() == 1);
    CHECK(w.tileIndex[30 * 64 + 33] == 0);
    CHECK(w.tileIndex[0] == -1);
    const WdlTile& t = w.tiles[0];
    CHECK(t.minHeight == -40);
    CHECK(t.maxHeight == 300);
    CHECK(t.holes[2] == 0x05);

    // 0x007d5150: the outer grid from the tile's north-west corner, the inner
    // half a cell in. Row 30 is X 17066.666 - 30 * 533.333.
    const auto v = wdl::tileVertices(t, 30, 33);
    CHECK(v[0] == Catch::Approx(17066.666f - 30 * 16 * 33.333332f));
    CHECK(v[1] == Catch::Approx(17066.666f - 33 * 16 * 33.333332f));
    CHECK(v[5 * 3 + 1] == Catch::Approx(v[1] - 5 * 33.333332f));
    CHECK(v[5 * 3 + 2] == -40.0f);
    CHECK(v[17 * 3 + 0] == Catch::Approx(v[0] - 33.333332f));
    CHECK(v[289 * 3 + 0] == Catch::Approx(v[0] - 16.666666f));
    CHECK(v[289 * 3 + 1] == Catch::Approx(v[1] - 16.666666f));
    CHECK(v[400 * 3 + 2] == 300.0f);
}

TEST_CASE("a .wdl without MAOF reads as no horizon", "[wdl]") {
    std::vector<uint8_t> d;
    putTag(d, "MVER"); put32(d, 4); put32(d, 18);
    CHECK(parseWDL(d).empty());
    CHECK(parseWDL({}).empty());
}

TEST_CASE("each cell is four triangles about its centre, as 0x007d5240 lists them", "[wdl]") {
    const auto idx = wdl::tileIndices();
    // The first cell: (c, a+1, a), (c, a+18, a+1), (c, a+17, a+18), (c, a, a+17).
    const uint16_t first[12] = {289, 1, 0, 289, 18, 1, 289, 17, 18, 289, 0, 17};
    for (int i = 0; i < 12; ++i) CHECK(idx[i] == first[i]);
    // The second row's first cell starts at outer vertex 17, centre 305.
    const size_t row1 = 16 * 12;
    CHECK(idx[row1 + 0] == 305);
    CHECK(idx[row1 + 2] == 17);
    // The last cell.
    CHECK(idx[wdl::kIndices - 3] == 288 + 256);
    CHECK(idx[wdl::kIndices - 2] == 15 * 17 + 15);
    CHECK(idx[wdl::kIndices - 1] == 16 * 17 + 15);
}

TEST_CASE("the tiles visited reach scale x far clip and two tiles more", "[wdl]") {
    // Mulgore, fc168's spot: X -2219.9, Y -697.5, far clip 1277, scale 4.
    const auto r = wdl::visibleTiles(-2219.899f, -697.470f, 1277.0f, 4.0f);
    // Cell (17066.666 + 2219.9) * 0.03 - 0.5 = 578.1 -> 578; reach 153.
    CHECK(r.rowMin == ((578 - 153) >> 4) - 2);
    CHECK(r.rowMax == ((578 + 153) >> 4) + 2);
    const int col = 532;  // (17066.666 + 697.47) * 0.03 - 0.5 = 532.4
    CHECK(r.colMin == ((col - 153) >> 4) - 2);
    CHECK(r.colMax == ((col + 153) >> 4) + 2);
    // Clamped to the map.
    const auto edge = wdl::visibleTiles(17000.0f, 17000.0f, 1277.0f, 6.0f);
    CHECK(edge.rowMin == 0);
    CHECK(edge.colMin == 0);
}

TEST_CASE("horizonFarclipScale is kept to 3..6 and sets the horizon's planes", "[wdl]") {
    CHECK(wdl::clampHorizonScale(4.0f) == 4.0f);
    CHECK(wdl::clampHorizonScale(1.0f) == 3.0f);
    CHECK(wdl::clampHorizonScale(9.0f) == 6.0f);
    CHECK(wdl::horizonNear(1277.0f) == 1227.0f);
    CHECK(wdl::horizonFar(1277.0f, 4.0f) == 5108.0f);
}
