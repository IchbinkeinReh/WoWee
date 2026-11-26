// A placed building's liquids, whoever placed it (U17): the client draws a
// group's liquid with the group (0x00793d20), and a map that is one building
// (its WDT's MODF, 0x007bfce0) places it as a map object like a tile's. Molten
// Core's lava was never handed to the water renderer: only the tile path
// placed liquids.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"

#include "rendering/wmo_liquid_load.hpp"

#include <string>
#include <vector>

using namespace wowee;
using rendering::wmo_liquid_load::placeGroupLiquids;

namespace {

pipeline::WMOModel modelWithLiquidIn(const std::vector<int>& liquidGroups, size_t groupCount) {
    pipeline::WMOModel model;
    model.groups.resize(groupCount);
    pipeline::WMOMaterial mat{};
    mat.color2 = 0xff3060a0u;
    model.materials.push_back(mat);
    for (int g : liquidGroups) {
        auto& liquid = model.groups[static_cast<size_t>(g)].liquid;
        liquid.xVerts = 3;
        liquid.yVerts = 2;
        liquid.xTiles = 2;
        liquid.yTiles = 1;
        liquid.momtIndex = static_cast<uint16_t>(g == 0 ? 0 : 7);  // 7: names no material
    }
    return model;
}

struct Placed {
    uint32_t group;
    uint32_t color;
};

}  // namespace

TEST_CASE("every group with a liquid is placed, with MLIQ's MOMT colour", "[wmo_liquid_load]") {
    const auto model = modelWithLiquidIn({0, 2, 3}, 5);
    std::vector<Placed> placed;
    const size_t next = placeGroupLiquids(model, 0, model.groups.size(),
        [&](const pipeline::WMOLiquid&, uint32_t color, uint32_t group) { placed.push_back({group, color}); });
    CHECK(next == 5);
    REQUIRE(placed.size() == 3);
    CHECK(placed[0].group == 0);
    CHECK(placed[0].color == 0xff3060a0u);
    CHECK(placed[1].group == 2);
    CHECK(placed[1].color == 0xffffffffu);  // no such MOMT entry: white
    CHECK(placed[2].group == 3);
}

TEST_CASE("a budget stops after that many liquids and resumes from the next group", "[wmo_liquid_load]") {
    const auto model = modelWithLiquidIn({1, 2, 4}, 6);
    std::vector<uint32_t> placed;
    auto place = [&](const pipeline::WMOLiquid&, uint32_t, uint32_t group) { placed.push_back(group); };
    size_t next = placeGroupLiquids(model, 0, 2, place);
    CHECK(next == 3);
    CHECK(placed == std::vector<uint32_t>{1, 2});
    next = placeGroupLiquids(model, next, 2, place);
    CHECK(next == 6);
    CHECK(placed == std::vector<uint32_t>{1, 2, 4});
    CHECK(placeGroupLiquids(model, next, 2, place) == 6);  // done: nothing more
    CHECK(placed.size() == 3);
}

TEST_CASE("a map that is one building places its groups' liquids too", "[wmo_liquid_load]") {
    const std::string loader = test::slurp("src/core/world_loader.cpp");
    REQUIRE_FALSE(loader.empty());
    // The WMO-only branch, from the WDT check to the tile streaming.
    const size_t from = loader.find("if (isWMOOnlyMap) {");
    REQUIRE(from != std::string::npos);
    const size_t to = loader.find("} else {", loader.find("Failed to read root WMO file", from));
    REQUIRE(to != std::string::npos);
    const std::string branch = loader.substr(from, to - from);
    CHECK(branch.find("placeGroupLiquids") != std::string::npos);
    CHECK(branch.find("loadFromWMO") != std::string::npos);

    const std::string terrain = test::slurp("src/rendering/terrain_manager.cpp");
    REQUIRE_FALSE(terrain.empty());
    CHECK(terrain.find("placeGroupLiquids") != std::string::npos);
}
