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
    // Env on the first stage, T2 on the second.
    c = m2ResolveCombiner(0x0091, 2, 0);
    CHECK(c.source[0] == M2TexCoordSource::Env);
    CHECK(c.source[1] == M2TexCoordSource::UV1);
}

TEST_CASE("0x8000 ids: 0 is not drawn, 1..3 one opaque stage", "[m2]") {
    CHECK_FALSE(m2ResolveCombiner(0x8000, 1, 0).drawn);
    const auto c = m2ResolveCombiner(0x8001, 2, 0);
    CHECK(c.drawn);
    CHECK(c.stages == 1);
    CHECK(c.mode[0] == M2_COMBINE_OPAQUE);
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
    CHECK(m2PackCoordSources(c) == (2 | (1 << 2)));
}
