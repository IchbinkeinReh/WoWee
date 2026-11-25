// How an M2 batch combines its textures: the shader id the client gives it
// when the skin loads (0x00836980), and the stages that id selects
// (0x00836600, 0x00836c90).
#include <catch_amalgamated.hpp>

#include "rendering/m2_texture_combiner.hpp"

using namespace wowee::rendering;

TEST_CASE("a model without combiner combos: blended or not, and its coordinate", "[m2]") {
    const std::vector<uint16_t> coords = {0, 1, 0xFFFF};
    const std::vector<uint16_t> none;
    // Opaque material on UV0.
    CHECK(m2BatchShaderId(0, 0, 1, 0, 0, coords, none) == 0x0000);
    // Blended material: mode 1 in the high nibble.
    CHECK(m2BatchShaderId(0, 2, 1, 0, 0, coords, none) == 0x0010);
    // UV1 sets 0x4000; the env map sets 8 in the nibble.
    CHECK(m2BatchShaderId(0, 2, 1, 1, 0, coords, none) == 0x4010);
    CHECK(m2BatchShaderId(0, 0, 1, 2, 0, coords, none) == 0x0080);
    // A stored id with 0x8000 is kept as it is.
    CHECK(m2BatchShaderId(0x8002, 2, 1, 0, 0, coords, none) == 0x8002);
}

TEST_CASE("a model with combiner combos builds the id from each texture", "[m2]") {
    const std::vector<uint16_t> coords = {0, 1, 0xFFFF, 0};
    const std::vector<uint16_t> combos = {1, 3, 4, 4};
    // Blended, two textures: Mod then Add, the second on UV1.
    CHECK(m2BatchShaderId(0, 2, 2, 0, 0x8, coords, combos) == 0x4013);
    // Opaque: the first stage is forced to 0.
    CHECK(m2BatchShaderId(0, 0, 2, 0, 0x8, coords, combos) == 0x4003);
    // Env map on the second texture.
    CHECK(m2BatchShaderId(2, 2, 2, 1, 0x8, coords, combos) == 0x004C);
}

TEST_CASE("one texture: its mode and where its coordinates come from", "[m2]") {
    auto c = m2ResolveCombiner(0x0010, 1, 0);
    CHECK(c.drawn);
    CHECK(c.stages == 1);
    CHECK(c.mode[0] == M2_COMBINE_MOD);
    CHECK(c.source[0] == M2TexCoordSource::UV0);
    c = m2ResolveCombiner(0x4000, 1, 1);
    CHECK(c.mode[0] == M2_COMBINE_OPAQUE);
    CHECK(c.source[0] == M2TexCoordSource::UV1);
    c = m2ResolveCombiner(0x0090, 1, 0xFFFF);
    CHECK(c.source[0] == M2TexCoordSource::Env);
    // Mod2xNA and AddNA have no single-texture shader of their own: Mod.
    CHECK(m2ResolveCombiner(0x0060, 1, 0).mode[0] == M2_COMBINE_MOD);
    CHECK(m2ResolveCombiner(0x0030, 1, 0).mode[0] == M2_COMBINE_ADD);
}

TEST_CASE("two textures: the client's pairs, and the fallback", "[m2]") {
    auto c = m2ResolveCombiner(0x0013, 2, 0);
    CHECK(c.stages == 2);
    CHECK(c.mode[0] == M2_COMBINE_MOD);
    CHECK(c.mode[1] == M2_COMBINE_ADD);
    CHECK(c.source[0] == M2TexCoordSource::UV0);
    CHECK(c.source[1] == M2TexCoordSource::UV1);
    // Opaque over Decal is drawn as Opaque_Mod.
    c = m2ResolveCombiner(0x0002, 2, 0);
    CHECK(c.mode[0] == M2_COMBINE_OPAQUE);
    CHECK(c.mode[1] == M2_COMBINE_MOD);
    // Mod2x over Mod is Combiners_Mod_Mod2x.
    c = m2ResolveCombiner(0x0041, 2, 0);
    CHECK(c.mode[0] == M2_COMBINE_MOD);
    CHECK(c.mode[1] == M2_COMBINE_MOD2X);
    // Add over Add has no shader: Mod_Mod on T1_T2, the env map dropped.
    c = m2ResolveCombiner(0x00BB, 2, 0);
    CHECK(c.mode[0] == M2_COMBINE_MOD);
    CHECK(c.mode[1] == M2_COMBINE_MOD);
    CHECK(c.source[0] == M2TexCoordSource::UV0);
    CHECK(c.source[1] == M2TexCoordSource::UV1);
    // Env on the first stage; Diffuse_Env_T2 then reads the first UV set.
    c = m2ResolveCombiner(0x0091, 2, 0);
    CHECK(c.source[0] == M2TexCoordSource::Env);
    CHECK(c.source[1] == M2TexCoordSource::UV0Raw);
}

TEST_CASE("0x8000 ids: 0 is not drawn, 1..3 their own programs on T1 and Env", "[m2]") {
    CHECK_FALSE(m2ResolveCombiner(0x8000, 1, 0).drawn);
    // Combiners_Opaque_Mod2xNA_Alpha, _AddAlpha, _AddAlpha_Alpha on
    // Diffuse_T1_Env (0x00836c90).
    for (uint16_t i = 1; i <= 3; ++i) {
        const auto c = m2ResolveCombiner(static_cast<uint16_t>(0x8000 | i), 2, 0);
        CHECK(c.drawn);
        CHECK(c.stages == 2);
        CHECK(c.mode[0] == M2_COMBINE_OPAQUE);
        CHECK(c.mode[1] == M2_COMBINE_MOD2X_NA_ALPHA + (i - 1));
        CHECK(c.source[0] == M2TexCoordSource::UV0);
        CHECK(c.source[1] == M2TexCoordSource::Env);
    }
    // With one texture there is no second stage to draw.
    CHECK(m2ResolveCombiner(0x8002, 1, 0).stages == 1);
}

TEST_CASE("Diffuse_Env_T2's second stage is the first UV set, untransformed", "[m2]") {
    const auto c = m2ResolveCombiner(0x0081, 2, 0);
    CHECK(c.source[0] == M2TexCoordSource::Env);
    CHECK(c.source[1] == M2TexCoordSource::UV0Raw);
    CHECK(m2PackCoordSources(c) == (2 | (3 << 2)));
    // Diffuse_T1_Env: the env map second.
    const auto d = m2ResolveCombiner(0x0018, 2, 0);
    CHECK(d.source[0] == M2TexCoordSource::UV0);
    CHECK(d.source[1] == M2TexCoordSource::Env);
}

TEST_CASE("the ops each mode configures, from the exe's tables", "[m2]") {
    // 0x00af5a08 colour, 0x00af59e8 alpha.
    CHECK(kM2CombinerColorOp[M2_COMBINE_OPAQUE] == M2_OP_MOD);
    CHECK(kM2CombinerAlphaOp[M2_COMBINE_OPAQUE] == M2_OP_PASS);
    CHECK(kM2CombinerColorOp[M2_COMBINE_ADD] == M2_OP_ADD);
    CHECK(kM2CombinerAlphaOp[M2_COMBINE_ADD] == M2_OP_ADD);
    CHECK(kM2CombinerColorOp[M2_COMBINE_MOD2X_NA] == M2_OP_MOD2X);
    CHECK(kM2CombinerAlphaOp[M2_COMBINE_MOD2X_NA] == M2_OP_PASS);
    CHECK(kM2CombinerColorOp[M2_COMBINE_DECAL] == M2_OP_DECAL);
    CHECK(kM2CombinerColorOp[M2_COMBINE_FADE] == M2_OP_FADE);
}

TEST_CASE("packing for the shaders", "[m2]") {
    const auto c = m2ResolveCombiner(0x0094, 2, 0);
    CHECK(m2PackCombinerModes(c) == (M2_COMBINE_MOD | (M2_COMBINE_MOD2X << 4) | (2 << 8)));
    CHECK(m2PackCoordSources(c) == (2 | (3 << 2)));
}

namespace {
struct Material {
    uint16_t flags = 0, blendMode = 0;
};
struct Batch {
    uint16_t shader = 0, materialIndex = 0, materialLayer = 0, textureCount = 1, textureIndex = 0, textureUnit = 0,
             transparencyIndex = 0, textureAnimIndex = 0, foldedShader = 0xFFFF;
};
struct Model {
    uint32_t globalFlags = 0;
    std::vector<Batch> batches;
    std::vector<Material> materials;
    std::vector<uint16_t> textureLookup, textureCoordCombos, textureCombinerCombos, textureTransformLookup,
        textureWeightLookup;
};
}  // namespace

// 0x00837680, with Polearm_2H_Lance_A_01's data: drawn see-through it was twice as opaque as who
// held it.
TEST_CASE("a reflecting layer and its texture again over it are one", "[m2]") {
    Model m{.globalFlags = 8,
            .batches = {{.materialIndex = 0, .textureCount = 2}, {.materialIndex = 1, .materialLayer = 1}},
            .materials = {{0, 0}, {0, 1}},
            .textureLookup = {0, 1},
            .textureCoordCombos = {0, 0xFFFF},
            .textureCombinerCombos = {1, 4},
            .textureTransformLookup = {0xFFFF, 0xFFFF},
            .textureWeightLookup = {0}};
    const Model as = m;
    m2FoldLayers(m);
    CHECK(m2ShaderIdOf(m, m.batches[0]) == 0x8001);
    CHECK(m2ShaderIdOf(m, m.batches[1]) == 0x8000);
    CHECK_FALSE(m2ResolveCombiner(0x8000, 1, 0).drawn);
    CHECK(m2ResolveCombiner(m2ShaderIdOf(m, m.batches[0]), m.batches[0].textureCount, 0).mode[1] ==
          M2_COMBINE_MOD2X_NA_ALPHA);
    // Not one unlit over a lit one, another's texture, another weight, or an added one.
    const auto stays = [&](Model with) {
        m2FoldLayers(with);
        return m2ShaderIdOf(with, with.batches[0]) == 0x000C && m2ShaderIdOf(with, with.batches[1]) != 0x8000;
    };
    Model other = as;
    other.materials[1].flags = 1;
    CHECK(stays(other));
    other = as;
    other.batches[1].textureIndex = 1;
    CHECK(stays(other));
    other = as;
    other.textureWeightLookup = {0, 1};
    other.batches[1].transparencyIndex = 1;
    CHECK(stays(other));
    other = as;
    other.materials[1].blendMode = 4;
    CHECK(stays(other));
    // Nothing over another: left alone.
    other = as;
    other.batches.pop_back();
    m2FoldLayers(other);
    CHECK(other.batches[0].foldedShader == 0xFFFF);
}

TEST_CASE("a reflection added or doubled over a texture is its second texture", "[m2]") {
    Model m{.batches = {{.materialIndex = 0, .textureIndex = 0, .textureUnit = 0},
                        {.materialIndex = 1, .materialLayer = 1, .textureIndex = 1, .textureUnit = 1, .textureAnimIndex = 1},
                        {.materialIndex = 2, .materialLayer = 2, .textureIndex = 0, .textureUnit = 0}},
            .materials = {{0, 0}, {0, 4}, {0, 2}},
            .textureLookup = {3, 5},
            .textureCoordCombos = {0, 0xFFFF},
            .textureTransformLookup = {0xFFFF, 2},
            .textureWeightLookup = {0}};
    Model two = m;
    two.batches.pop_back();
    m2FoldLayers(two);
    CHECK(two.batches[0].foldedShader == 0x8002);
    CHECK(two.batches[1].foldedShader == 0x8000);
    REQUIRE(two.batches[0].textureCount == 2);
    CHECK(two.textureLookup[two.batches[0].textureIndex] == 3);
    CHECK(two.textureLookup[two.batches[0].textureIndex + 1u] == 5);
    CHECK(two.textureTransformLookup[two.batches[0].textureAnimIndex] == 0xFFFF);
    CHECK(two.textureTransformLookup[two.batches[0].textureAnimIndex + 1u] == 2);
    // Doubled: the program of a texture and a reflection doubled over it.
    two = m;
    two.batches.pop_back();
    two.materials[1].blendMode = 6;
    m2FoldLayers(two);
    CHECK(two.batches[0].foldedShader == 0x000E);
    // And the texture once more over that.
    Model three = m;
    m2FoldLayers(three);
    CHECK(three.batches[0].foldedShader == 0x8003);
    CHECK(three.batches[2].foldedShader == 0x8000);
    three = m;
    three.materials[1].blendMode = 6;
    m2FoldLayers(three);
    CHECK(three.batches[0].foldedShader == 0x8001);
}

TEST_CASE("a batch of the material before it is drawn as that one", "[m2]") {
    Model m{.batches = {{.materialIndex = 0}, {.materialIndex = 1, .materialLayer = 1, .textureIndex = 1, .textureUnit = 1},
                        {.materialIndex = 0}, {.materialIndex = 0, .textureIndex = 1}},
            .materials = {{0, 0}, {0, 4}},
            .textureLookup = {0, 1},
            .textureCoordCombos = {0, 0xFFFF},
            .textureTransformLookup = {0xFFFF, 0xFFFF},
            .textureWeightLookup = {0}};
    m2FoldLayers(m);
    CHECK(m.batches[0].foldedShader == 0x8002);
    CHECK(m.batches[2].foldedShader == 0x0000);
    CHECK(m.batches[3].foldedShader == 0x0000);
    CHECK(m.batches[3].textureIndex == m.batches[2].textureIndex);
}
