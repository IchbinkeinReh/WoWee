// A placed doodad whose model is emitters alone - no vertices, no skin - is
// kept and drawn, as the client draws the smoke and steam columns
// (BLACKSMITH_SMOKE, STEAM02-04, BUBBLESB_BIGEMITTER) over forges, ships and
// Borean Tundra's pools. A model with nothing at all is still dropped.
//
// So is a unit's, and what it wears or holds: the marshes' gas clouds
// (Creature\Cloud\CloudSwampGas.m2, no vertex and four emitters), a weapon's
// glow (MongooseGlow_High), a helm's (Helm_Robe_RaidPriest_C_01). All of them
// were turned down as broken.
#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using wowee::pipeline::M2Model;

namespace {

std::map<std::string, std::vector<uint8_t>> files;

/// A version 264 model with `emitters` particle emitters and nothing else.
std::vector<uint8_t> emitterModel(uint32_t emitters) {
    constexpr size_t kHeader = 0x138, kEmitter = 0x1dc;
    std::vector<uint8_t> bytes(kHeader + kEmitter * emitters, 0);
    const auto put = [&](size_t at, uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); };
    put(0x00, 0x3032444d);  // MD20
    put(0x04, 264);
    put(0x128, emitters);
    put(0x12c, kHeader);
    return bytes;
}

std::string source(const char* path) {
    std::ifstream in(std::string(WOWEE_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// What the loader reads its files through, for this test.
namespace wowee::pipeline {
std::vector<uint8_t> AssetManager::readFile(const std::string& path) const {
    const auto it = files.find(path);
    return it == files.end() ? std::vector<uint8_t>{} : it->second;
}
std::vector<uint8_t> AssetManager::readFileOptional(const std::string& path) const { return readFile(path); }
}  // namespace wowee::pipeline

TEST_CASE("an emitter-only model is drawable, an empty one is not", "[m2][doodad]") {
    M2Model empty{};
    CHECK_FALSE(empty.isValid());
    CHECK_FALSE(empty.hasDrawableContent());

    M2Model smoke{};
    smoke.particleEmitters.resize(2);
    CHECK_FALSE(smoke.isValid());
    CHECK(smoke.hasDrawableContent());

    M2Model trail{};
    trail.ribbonEmitters.resize(1);
    CHECK(trail.hasDrawableContent());

    M2Model mesh{};
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    CHECK(mesh.isValid());
    CHECK(mesh.hasDrawableContent());
}

TEST_CASE("a model of emitters alone loads as any other does", "[m2]") {
    // Never built: only the two functions above are called, and they read no
    // member.
    alignas(wowee::pipeline::AssetManager) static std::byte raw[sizeof(wowee::pipeline::AssetManager)];
    auto& assets = *reinterpret_cast<wowee::pipeline::AssetManager*>(raw);
    files["Spells\\Enchantments\\MongooseGlow_High.m2"] = emitterModel(3);
    files["Creature\\Empty.m2"] = emitterModel(0);

    M2Model glow{};
    REQUIRE(wowee::pipeline::loadM2WithSkin(assets, "Spells\\Enchantments\\MongooseGlow_High.m2", glow));
    CHECK(glow.particleEmitters.size() == 3);
    CHECK(glow.vertices.empty());
    M2Model again{};
    CHECK(wowee::pipeline::loadM2WithAllAnimations(assets, "Spells\\Enchantments\\MongooseGlow_High.m2", again));
    M2Model none{};
    CHECK_FALSE(wowee::pipeline::loadM2WithSkin(assets, "Creature\\Empty.m2", none));
    CHECK_FALSE(wowee::pipeline::loadM2WithSkin(assets, "Creature\\Missing.m2", none));
}

TEST_CASE("nothing that draws a unit or its items asks for a vertex", "[m2]") {
    // The renderer of units and the worn and held models it attaches.
    const std::string renderer = source("src/rendering/character_renderer.cpp");
    const size_t load = renderer.find("bool CharacterRenderer::loadModel(");
    REQUIRE(load != std::string::npos);
    const std::string head = renderer.substr(load, 400);
    CHECK(head.find("hasDrawableContent()") != std::string::npos);
    CHECK(head.find("model.isValid()") == std::string::npos);

    const std::string spawner = source("src/core/entity_spawner_processing.cpp");
    REQUIRE(!spawner.empty());
    for (const char* turnedDown : {"if (model->vertices.empty()) {", "if (model.vertices.empty()) {",
                                   "if (model.vertices.empty()) return", "if (!model.isValid()) return"}) {
        INFO(turnedDown);
        CHECK(spawner.find(turnedDown) == std::string::npos);
    }
    for (const char* path : {"src/core/item_attachments.cpp", "src/core/asset_prefetch.cpp",
                             "src/core/appearance_composer.cpp", "src/core/entity_spawner_player.cpp",
                             "src/rendering/model_frame_view.cpp"}) {
        const std::string text = source(path);
        INFO(path);
        REQUIRE(!text.empty());
        CHECK(text.find("odel.isValid()") == std::string::npos);
        CHECK(text.find("odel->isValid()") == std::string::npos);
        CHECK(text.find("out.isValid()") == std::string::npos);
    }
}
