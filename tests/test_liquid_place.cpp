// Which liquid a point is in, by the place it is in (0x00790920, 0x007a1bc0):
// a building group's own inside one, else the exterior groups', else the
// ground's. The Coilfang entrance rooms lie ninety yards under their lake and
// are dry; asked by x and y alone they were drawn as under water.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"

#include "rendering/collision_liquid.hpp"
#include "rendering/liquid_place.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace wowee::rendering;
using liquid_place::Liquid;

namespace {

struct Scene {
    std::map<int, Liquid> groups;       // by group: its liquid over the point
    std::vector<int> exterior;          // the exterior groups holding the point
    std::optional<Liquid> ground;
    std::optional<Liquid> at(std::optional<int> in) const {
        return liquid_place::liquidOf(
            in,
            [&](int g) -> std::optional<Liquid> {
                const auto it = groups.find(g);
                return it == groups.end() ? std::nullopt : std::optional<Liquid>(it->second);
            },
            [&] { return exterior; }, [&] { return ground; });
    }
};

const Liquid kLake{18.3f, 5, false};

}  // namespace

TEST_CASE("a building's group without liquid is dry under a lake", "[liquid]") {
    Scene s;
    s.ground = kLake;
    CHECK_FALSE(s.at(7));
    // An exterior group's water over the room is not the room's either.
    s.exterior = {3};
    s.groups[3] = Liquid{2.0f, 13, true};
    CHECK_FALSE(s.at(7));
}

TEST_CASE("a building's group has its own liquid and no other", "[liquid]") {
    Scene s;
    s.ground = kLake;
    s.groups[7] = Liquid{-70.0f, 13, true};
    const auto in = s.at(7);
    REQUIRE(in);
    CHECK(in->level == -70.0f);
    CHECK(in->liquidType == 13);
    CHECK(in->building);
}

TEST_CASE("outside, the exterior groups' liquid comes before the ground's", "[liquid]") {
    Scene s;
    s.ground = kLake;
    REQUIRE(s.at(std::nullopt));
    CHECK(s.at(std::nullopt)->liquidType == 5);
    s.exterior = {1, 3};
    s.groups[3] = Liquid{2.0f, 13, true};
    CHECK(s.at(std::nullopt)->level == 2.0f);
    s = Scene{};
    CHECK_FALSE(s.at(std::nullopt));
}

TEST_CASE("the ground's liquid is not under the ground", "[liquid]") {
    // The camera in the Coilfang room, if no group were found round it: the
    // bed of the lake lies over it (0x007a0820 with the ground asked).
    CHECK_FALSE(collision::groundLiquidHolds(-71.0f, 18.3f, -40.0f));
    CHECK(collision::groundLiquidHolds(-30.0f, 18.3f, -40.0f));
}

TEST_CASE("a group with a liquid and no tiles is liquid throughout", "[liquid]") {
    // MD_UnderwaterCave's groups: MOGP liquid 0 (row 13), no MLIQ.
    CHECK(liquid_place::groupFilled(13, 0, 0));
    CHECK_FALSE(liquid_place::groupFilled(13, 4, 4));
    CHECK_FALSE(liquid_place::groupFilled(0, 0, 0));
    CHECK(Liquid{liquid_place::kFilledLevel, 13, true}.filled());
    CHECK_FALSE(kLake.filled());
}

TEST_CASE("the places are looked for only where there can be liquid", "[liquid]") {
    CHECK(liquid_place::mayBeInLiquid(18.3f, -73.0f, false));
    CHECK_FALSE(liquid_place::mayBeInLiquid(18.3f, 20.0f, false));
    CHECK_FALSE(liquid_place::mayBeInLiquid(std::nullopt, 0.0f, false));
    CHECK(liquid_place::mayBeInLiquid(std::nullopt, 0.0f, true));
}

TEST_CASE("nothing asks what it is in by x and y alone", "[liquid]") {
    // The x and y queries answer the surface over or under a point, which is
    // not what the point is in: that is liquid_query's. What is left of them:
    // the free camera and the spawn search, the grass, the rain, the boats and
    // the probes.
    const std::map<std::string, int> allowedHeight{{"src/rendering/camera_controller.cpp", 2},
                                                   {"src/rendering/renderer.cpp", 1},
                                                   {"src/rendering/renderer_wave_ride.cpp", 1},
                                                   {"src/rendering/liquid_query.cpp", 1},
                                                   {"src/mcp/tools_terrain.cpp", 1},
                                                   {"src/rendering/water_renderer.cpp", 1}};
    const std::map<std::string, int> allowedNearest{{"src/rendering/renderer.cpp", 1},
                                                    {"src/rendering/water_renderer.cpp", 1}};
    const auto count = [](const std::string& text, const std::string& word) {
        int n = 0;
        for (size_t p = text.find(word); p != std::string::npos; p = text.find(word, p + 1)) ++n;
        return n;
    };
    const std::filesystem::path root = WOWEE_SOURCE_DIR;
    size_t files = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root / "src")) {
        if (e.path().extension() != ".cpp") continue;
        ++files;
        std::ifstream in(e.path(), std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        const std::string name = std::filesystem::relative(e.path(), root).generic_string();
        INFO(name);
        const auto h = allowedHeight.find(name);
        CHECK(count(text, "getWaterHeightAt(") == (h == allowedHeight.end() ? 0 : h->second));
        const auto n = allowedNearest.find(name);
        CHECK(count(text, "getNearestWaterHeightAt(") == (n == allowedNearest.end() ? 0 : n->second));
    }
    CHECK(files > 200);
}
