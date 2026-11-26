// A placed building's liquids, whoever placed it (U17): the client draws a
// group's liquid with the group (0x00793d20), and a map that is one building
// (its WDT's MODF, 0x007bfce0) places it as a map object like a tile's. Molten
// Core's lava was never handed to the water renderer: only the tile path
// placed liquids.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"

#include "rendering/client_liquid.hpp"
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

// T375: Molten Core's magma turned pale lavender far off. Its group's
// CClientEnvironment_Liquid (0x007d5120) takes its fog in 0x007d4f40 by the
// group's 0x8000 (0x007964a0): clear - the group in the camera's interior
// pass - the camera's fog with the interior's MFOG in it (0xd38b8c), set the
// zone's (0xd38ba0). The client's liquid draw took the zone's always.
TEST_CASE("a building group's liquid takes the fog its group is drawn in (T375)", "[wmo_liquid_load]") {
    using rendering::client_liquid::liquidTakesCameraFog;
    CHECK(liquidTakesCameraFog(true, true));
    CHECK_FALSE(liquidTakesCameraFog(true, false));
    CHECK_FALSE(liquidTakesCameraFog(false, true));

    const std::string frag = test::slurp("assets/shaders/liquid.frag.glsl");
    REQUIRE_FALSE(frag.empty());
    CHECK(frag.find("vec4 cameraFogColor;") != std::string::npos);
    CHECK(frag.find("push.scroll.z > 0.5 ? cameraFogColor.rgb : fogColor.rgb") != std::string::npos);

    const std::string water = test::slurp("src/rendering/water_renderer.cpp");
    REQUIRE_FALSE(water.empty());
    CHECK(water.find("push.scroll.z = client_liquid::liquidTakesCameraFog(") != std::string::npos);
    // The procedural water too (psLiquidProcWater): its own push constant.
    const std::string proc = test::slurp("assets/shaders/liquid_proc.frag.glsl");
    const std::string procBlock = test::slurp("assets/shaders/liquid_proc.glsli");
    REQUIRE_FALSE(proc.empty());
    CHECK(procBlock.find("vec4 cameraFogColor;") != std::string::npos);
    CHECK(proc.find("procPush.fog.x > 0.5 ? cameraFogColor.rgb : fogColor.rgb") != std::string::npos);
    CHECK(water.find("const glm::vec4 procFog(client_liquid::liquidTakesCameraFog(") != std::string::npos);

    const std::string renderer = test::slurp("src/rendering/renderer.cpp");
    REQUIRE_FALSE(renderer.empty());
    CHECK(renderer.find("groupInInteriorPass(instanceId, group)") != std::string::npos);
}

// T376: Molten Core's lava stands 71 yards over its MLIQ's base
// (Blackrock_lower_guild_003.wmo: base -228.9, the river -157.3), and the
// water renderer kept a vertex's height only within 8 yards of the base: the
// swim, camera and fog queries all took the base, and no one swam in the lava.
// The group's liquid search reads a liquid tile's four corners (0x0079b760),
// whatever their height.
TEST_CASE("a liquid tile's corners keep their heights, however far over the base (T376)", "[wmo_liquid_load]") {
    using rendering::wmo_liquid_load::usedVertices;
    // 3 x 2 tiles; the middle of the top row has no liquid (0x0F), nor the
    // last of the bottom row (0x1F: the low nibble is what counts).
    const std::vector<uint8_t> flags{0x46, 0x0F, 0x46, 0x46, 0x46, 0x1F};
    const auto used = usedVertices(3, 2, flags);
    REQUIRE(used.size() == 4u * 3u);
    // Vertex rows: y 0, 1, 2; x 0..3.
    const std::vector<uint8_t> want{1, 1, 1, 1,   // tiles 0 and 2 use all four of the first row
                                    1, 1, 1, 1,
                                    1, 1, 1, 0};  // tile 5 has none: its far corner is no tile's
    CHECK(used == want);
    CHECK(usedVertices(2, 1, {0x0F, 0xFF}) == std::vector<uint8_t>(6, 0));

    const std::string water = test::slurp("src/rendering/water_renderer.cpp");
    REQUIRE_FALSE(water.empty());
    const size_t from = water.find("void WaterRenderer::loadFromWMO(");
    REQUIRE(from != std::string::npos);
    const std::string load = water.substr(from, water.find("void WaterRenderer::removeWMOs(", from) - from);
    CHECK(load.find("wmo_liquid_load::usedVertices(") != std::string::npos);
    CHECK(load.find("std::abs(z - surface.origin.z) <= 8.0f") == std::string::npos);
}
