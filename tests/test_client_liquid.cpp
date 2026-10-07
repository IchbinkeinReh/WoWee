// The client's own liquid: the arithmetic wow.exe 3.3.5a does to draw one,
// from rendering/client_liquid.hpp. Every case is a number the exe states -
// a table it fills, a constant it multiplies by - so a change here is a
// change away from the client.

#include <catch_amalgamated.hpp>

#include "rendering/client_liquid.hpp"

#include <cmath>

namespace cl = wowee::rendering::client_liquid;

TEST_CASE("a LiquidMaterial id picks the client's material class", "[liquid]") {
    // 0x008a1fa0: 1 water, 2 magma, 3 procedural water, anything else none.
    CHECK(cl::materialKind(1) == cl::MaterialKind::Water);
    CHECK(cl::materialKind(2) == cl::MaterialKind::Magma);
    CHECK(cl::materialKind(3) == cl::MaterialKind::ProcWater);
    CHECK(cl::materialKind(0) == cl::MaterialKind::None);
    CHECK(cl::materialKind(4) == cl::MaterialKind::None);
}

TEST_CASE("the fixed-function materials read the slots the exe names", "[liquid]") {
    // 0x008a5c70: depth slot 1, animated slot 0 over Int[1]; Float[2] scales
    // the depth coordinate, Float[0] and Float[1] the surface's.
    CHECK(cl::kWaterStages.depthSlot == 1);
    CHECK(cl::kWaterStages.animSlot == 0);
    CHECK(cl::kWaterStages.animPeriodInt == 1);
    CHECK(cl::kWaterStages.depthScaleFloat == 2);
    CHECK(cl::kWaterStages.scaleFloat == 0);
    CHECK(cl::kWaterStages.rotationFloat == 1);
    // 0x008a5170: slots 4 and 5, Int[2], Floats 8, 9, 10.
    CHECK(cl::kProcWaterStages.depthSlot == 4);
    CHECK(cl::kProcWaterStages.animSlot == 5);
    CHECK(cl::kProcWaterStages.animPeriodInt == 2);
    CHECK(cl::kProcWaterStages.depthScaleFloat == 8);
    CHECK(cl::kProcWaterStages.scaleFloat == 9);
    CHECK(cl::kProcWaterStages.rotationFloat == 10);
    CHECK(cl::kFixedSlotPeriodMs == 0x4e2);
}

TEST_CASE("an animated slot steps through its frames once a period", "[liquid]") {
    // 0x008a1d60: round(frames x elapsed / period - 0.5).
    CHECK(cl::animFrameIndex(0, 1000, 30) == 0);
    CHECK(cl::animFrameIndex(999, 1000, 30) == 29);
    CHECK(cl::animFrameIndex(1000, 1000, 30) == 0);   // wrapped
    CHECK(cl::animFrameIndex(500, 1000, 30) == 14);   // 14.5 rounds to even
    CHECK(cl::animFrameIndex(510, 1000, 30) == 15);
    CHECK(cl::animFrameIndex(34, 1000, 30) == 1);     // 1.02 - 0.5
    // One frame, or a zero period, cannot leave the first.
    CHECK(cl::animFrameIndex(12345, 1000, 1) == 0);
    CHECK(cl::animFrameIndex(12345, 0, 30) < 30);
}

TEST_CASE("the depth byte becomes the coordinate 0x0079e3c0 tabulates", "[liquid]") {
    // Table 0: a ninth of the depth times 0.21428572, 1 past 4.6666665 ninths.
    CHECK(cl::depthCoord(0, 0) == 0.0f);
    CHECK(cl::depthCoord(21, 0) == Catch::Approx(0.5f).margin(1e-4));
    CHECK(cl::depthCoord(42, 0) == Catch::Approx(1.0f).margin(1e-4));
    CHECK(cl::depthCoord(43, 0) == 1.0f);
    CHECK(cl::depthCoord(255, 0) == 1.0f);
    // Table 1: over the whole byte.
    CHECK(cl::depthCoord(0, 1) == 0.0f);
    CHECK(cl::depthCoord(51, 1) == Catch::Approx(0.2f).margin(1e-4));
    CHECK(cl::depthCoord(255, 1) == 1.0f);
    // Any other index has no table.
    CHECK(cl::depthCoord(100, 2) == 0.0f);
}

TEST_CASE("only a material whose vertices carry depth gets a depth coordinate", "[liquid]") {
    // 0x0079b870: LVF 0 or 2, and a table it has.
    CHECK(cl::hasDepthCoord(0, 0));
    CHECK(cl::hasDepthCoord(2, 1));
    CHECK_FALSE(cl::hasDepthCoord(1, 0));
    CHECK_FALSE(cl::hasDepthCoord(3, 0));
    CHECK_FALSE(cl::hasDepthCoord(0, 2));
}

TEST_CASE("a liquid's surface coordinate is laid down as the client lays it", "[liquid]") {
    // 0x007ce390: the world position times 0.06, on the client's axes - its
    // x is render y.
    const glm::vec2 uv = cl::chunkSurfaceCoord(glm::vec3(100.0f, 50.0f, 7.0f));
    CHECK(uv.x == Catch::Approx(3.0f));
    CHECK(uv.y == Catch::Approx(6.0f));
    // A stored coordinate: the ushort times 0.01171875.
    CHECK(cl::chunkStoredCoord(256, 128).x == Catch::Approx(3.0f));
    CHECK(cl::chunkStoredCoord(256, 128).y == Catch::Approx(1.5f));
    // 0x007a7b00: a WMO liquid repeats once a tile, and a stored one is over 256.
    CHECK(cl::wmoSurfaceCoord(3, 2).x == Catch::Approx(3.0f));
    CHECK(cl::wmoSurfaceCoord(3, 2).y == Catch::Approx(2.0f));
    CHECK(cl::wmoStoredCoord(512, -256).x == Catch::Approx(2.0f));
    CHECK(cl::wmoStoredCoord(512, -256).y == Catch::Approx(-1.0f));
}

TEST_CASE("the animated stage's matrix rotates by the float times 57.29578", "[liquid]") {
    // 0x004c3290 takes the product straight to fsincos, then 0x004c1bf0 scales.
    const glm::mat2 none = cl::animStageMatrix(0.0f, 2.0f);
    const glm::vec2 a = none * glm::vec2(1.0f, 0.5f);
    CHECK(a.x == Catch::Approx(2.0f));
    CHECK(a.y == Catch::Approx(1.0f));

    const float rot = 0.3f;
    const float angle = rot * 57.29578f;
    const glm::vec2 b = cl::animStageMatrix(rot, 1.0f) * glm::vec2(1.0f, 0.0f);
    // u' = u cos - v sin, v' = u sin + v cos.
    CHECK(b.x == Catch::Approx(std::cos(angle)).margin(1e-5));
    CHECK(b.y == Catch::Approx(std::sin(angle)).margin(1e-5));
}

TEST_CASE("magma scrolls a whole repeat every thousand over its speed", "[liquid]") {
    // 0x008a34b0.
    CHECK(cl::magmaScroll(0, 1.0f, 0.5f) == glm::vec2(0.0f));
    const glm::vec2 s = cl::magmaScroll(250, 1.0f, 0.5f);
    CHECK(s.x == Catch::Approx(0.25f));
    CHECK(s.y == Catch::Approx(0.125f));
    CHECK(cl::magmaScroll(1500, 1.0f, 0.0f).x == Catch::Approx(0.5f));
    CHECK(cl::magmaScroll(1500, 1.0f, 0.0f).y == 0.0f);
}

TEST_CASE("the depth ramp steps a 64th a row from the light's close colour", "[liquid]") {
    // 0x008a2bf0: 8.8 fixed point, the difference times 4 a row.
    const auto ramp = cl::depthRamp(glm::vec3(0.0f, 64.0f / 255.0f, 1.0f), 0.5f,
                                    glm::vec3(1.0f, 64.0f / 255.0f, 0.0f), 1.0f, false);
    CHECK(ramp[0] == cl::RampTexel{0, 64, 255, 128});
    // Row 32: halfway, in the client's own rounding.
    CHECK(ramp[32].r == 127);
    CHECK(ramp[32].g == 64);
    CHECK(ramp[32].b == 127);
    CHECK(ramp[32].a == 191);
    // Never reaching the far colour: row 63 is 63/64 of the way.
    CHECK(ramp[63].r == 251);
    CHECK(ramp[63].b == 3);
}

TEST_CASE("the ocean's deepest row is darker and opaque", "[liquid]") {
    // 0x008a2bf0, the last row when building the ocean's texture: to HSV,
    // value times 0.9, back, alpha 0xff.
    const auto river = cl::depthRamp(glm::vec3(0.2f), 0.3f, glm::vec3(0.6f), 0.6f, false);
    const auto ocean = cl::depthRamp(glm::vec3(0.2f), 0.3f, glm::vec3(0.6f), 0.6f, true);
    CHECK(river[62] == ocean[62]);
    CHECK(ocean[63].a == 0xff);
    CHECK(river[63].a != 0xff);
    CHECK(ocean[63].r == static_cast<uint8_t>(std::lrint(river[63].r * 0.9f)));
}

TEST_CASE("the WMO water texture is the river's far colour and white", "[liquid]") {
    // 0x008a2ac0: four texels of ch17 then four of white, the river's alpha
    // stepping down the rows.
    const auto t = cl::wmoWaterRamp(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, 1.0f);
    CHECK(t[0][0] == cl::RampTexel{255, 0, 0, 0});
    CHECK(t[0][1] == cl::RampTexel{255, 255, 255, 0});
    CHECK(t[32][0].a == 127);
    CHECK(t[32][1].a == 127);
}

TEST_CASE("a texture name is a sequence, a procedural texture or a file", "[liquid]") {
    // 0x008a2450.
    CHECK(cl::isFrameSequence("XTextures\\river\\lake_a.%d.blp"));
    CHECK_FALSE(cl::isFrameSequence("XTextures\\river\\lake_a.1.blp"));
    CHECK(cl::frameName("XTextures\\river\\lake_a.%d.blp", 7) == "XTextures\\river\\lake_a.7.blp");
    CHECK(cl::kMaxAnimFrames == 30);
    CHECK(cl::proceduralTexFor("proceduralRiverDepthTex") == cl::ProceduralTex::River);
    CHECK(cl::proceduralTexFor("proceduralOceanDepthTex") == cl::ProceduralTex::Ocean);
    CHECK(cl::proceduralTexFor("proceduralWmoWaterTex") == cl::ProceduralTex::WmoWater);
    CHECK(cl::proceduralTexFor("proceduralSomethingElse") == cl::ProceduralTex::Unknown);
    CHECK(cl::proceduralTexFor("XTextures\\lava\\lava.%d.blp") == cl::ProceduralTex::None);
}

TEST_CASE("a WMO liquid inside a building is drawn the interior way", "[liquid]") {
    // 0x00793d20: a group that is neither exterior (0x8) nor exterior-lit
    // (0x40), with a LiquidType lacking flag 0x200.
    CHECK(cl::wmoLiquidIsInterior(0x0, 0));
    CHECK_FALSE(cl::wmoLiquidIsInterior(0x8, 0));
    CHECK_FALSE(cl::wmoLiquidIsInterior(0x40, 0));
    CHECK_FALSE(cl::wmoLiquidIsInterior(0x0, 0x200));
    // The water types up to 20 become 17 there; the rest keep theirs.
    CHECK(cl::wmoInteriorLiquidType(1) == 17);
    CHECK(cl::wmoInteriorLiquidType(13) == 17);
    CHECK(cl::wmoInteriorLiquidType(2) == 2);
    CHECK(cl::wmoInteriorLiquidType(3) == 3);
    CHECK(cl::wmoInteriorLiquidType(21) == 21);
}
