// The rest of the original client's sky and world light (Wow.exe 3.3.5a,
// build 12340) that needs no GPU: the sun and moons' sizes and the Blue
// Child's clock, the glare's facing, the cloud dome and its texture's cover,
// the liquid darkening, the WMO interior fog, which sky models hide the
// procedural sky, and the MFOG / MCSH chunks as the loaders read them.
#include <catch_amalgamated.hpp>

#include "pipeline/adt_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/day_night.hpp"
#include "rendering/lighting_manager.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace dn = wowee::rendering::daynight;
using wowee::rendering::LightingManager;
using wowee::rendering::LightingParams;

TEST_CASE("the sun is drawn twice its size at dawn and dusk", "[sky][celestial]") {
    // 0xd39128: x2 at 06:00 and 21:00, x1 from 06:45 to 20:15.
    CHECK(dn::sampleCurve(dn::kSunSize, 0.25f) == Catch::Approx(2.0f));
    CHECK(dn::sampleCurve(dn::kSunSize, 0.5f) == Catch::Approx(1.0f));
    CHECK(dn::sampleCurve(dn::kSunSize, 0.875f) == Catch::Approx(2.0f));
    // The White Lady is 1.75 times the curve: 1.75 at midnight, 2.625 by day.
    CHECK(dn::sampleCurve(dn::kMoonSize, 0.0f) * dn::kWhiteLadyScale ==
          Catch::Approx(1.75f).margin(0.01f));
    CHECK(dn::sampleCurve(dn::kMoonSize, 0.5f) * dn::kWhiteLadyScale == Catch::Approx(2.625f));
}

TEST_CASE("the Blue Child runs on a clock 1.7 times slower", "[sky][celestial]") {
    // 0x007eecc0 with the day number 0xd38b08 left at zero.
    CHECK(dn::blueChildTime(0.0f) == Catch::Approx(0.0f));
    CHECK(dn::blueChildTime(0.85f) == Catch::Approx(0.5f));
    // Up just after midnight, low in the north, and set by mid-morning.
    CHECK(dn::blueChildDirection(0.01f).z > 0.3f);
    CHECK(dn::blueChildDirection(0.5f).z < 0.0f);
}

TEST_CASE("the glare grows as the camera turns to face the body", "[sky][glare]") {
    // 0x007ef6e0: nothing past 0.7, all of it looking straight at it.
    CHECK(dn::glareFacing(0.0f) == Catch::Approx(0.0f));
    CHECK(dn::glareFacing(0.7f) == Catch::Approx(0.0f));
    CHECK(dn::glareFacing(0.85f) == Catch::Approx(0.5f));
    CHECK(dn::glareFacing(1.0f) == Catch::Approx(1.0f));
    // The sun's glare shows by day only, the moon's by night only.
    CHECK(dn::sampleCurve(dn::kSunGlare.time, 0.5f) == Catch::Approx(1.0f));
    CHECK(dn::sampleCurve(dn::kSunGlare.time, 0.0f) == Catch::Approx(0.0f));
    CHECK(dn::sampleCurve(dn::kMoonGlare.time, 0.0f) == Catch::Approx(1.0f).margin(0.02f));
    CHECK(dn::sampleCurve(dn::kMoonGlare.time, 0.5f) == Catch::Approx(0.0f));
}

TEST_CASE("the cloud dome is solid overhead and gone at the horizon", "[sky][clouds]") {
    // 0xa41ad4 and 0xa41b04.
    CHECK(dn::kCloudDomeRows[0] == 0.0f);
    CHECK(dn::kCloudDomeRows[11] == 0.25f);
    CHECK(dn::kCloudDomeAlpha[0] == 255);
    CHECK(dn::kCloudDomeAlpha[9] == 128);
    CHECK(dn::kCloudDomeAlpha[11] == 0);
    // The texture is laid out by row, not by angle: row i is i/11 of the way.
    CHECK(dn::cloudTextureRadius(0.0f) == Catch::Approx(0.0f));
    CHECK(dn::cloudTextureRadius(0.1f) == Catch::Approx(0.5f * 4.0f / 11.0f));
    CHECK(dn::cloudTextureRadius(0.25f) == Catch::Approx(0.5f));
    // Below the horizon it runs on rather than stopping.
    CHECK(dn::cloudTextureRadius(0.3f) > 0.5f);
}

TEST_CASE("cloud cover is the noise over a threshold the density sets", "[sky][clouds]") {
    // 0x007efd00: (1 - density) x 255, and 255 - 255 x 0.96^(k x 153/256).
    CHECK(dn::cloudCoverageThreshold(0.0f) == Catch::Approx(255.0f));
    CHECK(dn::cloudCoverageThreshold(1.0f) == Catch::Approx(0.0f));
    CHECK(dn::cloudCoverageThreshold(0.6f) == Catch::Approx(102.0f));
    CHECK(dn::cloudCoverageThreshold(3.0f) == Catch::Approx(0.0f));
    CHECK(dn::cloudCoverageAlpha(-1.0f) == 0.0f);
    CHECK(dn::cloudCoverageAlpha(0.0f) == Catch::Approx(0.0f));
    CHECK(dn::cloudCoverageAlpha(28.3f) == Catch::Approx(0.5f).margin(0.01f));
    CHECK(dn::cloudCoverageAlpha(255.0f) > 0.99f);
    // Lit by the sun from 04:50 to 22:10, by the moon the rest of the night.
    CHECK(dn::cloudsLitBySun(0.5f));
    CHECK_FALSE(dn::cloudsLitBySun(0.1f));
    CHECK_FALSE(dn::cloudsLitBySun(0.95f));
    // Weather flattens and dims that light.
    CHECK(dn::cloudLightHeight(0.0f) == 64.0f);
    CHECK(dn::cloudLightHeight(1.0f) == 256.0f);
    CHECK(dn::cloudGlow(1.0f) == Catch::Approx(0.25f));
}

TEST_CASE("a liquid darkens the light with depth by its own row", "[sky][liquid]") {
    // 0x007f3230's tail: 1 - min(depth, max) / max x intensity.
    CHECK(dn::liquidDarkenScale(0.0f, 20.0f, 0.5f) == Catch::Approx(1.0f));
    CHECK(dn::liquidDarkenScale(10.0f, 20.0f, 0.5f) == Catch::Approx(0.75f));
    CHECK(dn::liquidDarkenScale(40.0f, 20.0f, 0.5f) == Catch::Approx(0.5f));
    CHECK(dn::liquidDarkenScale(40.0f, 20.0f, 2.0f) == Catch::Approx(0.0f));
    // A row with no depth set darkens nothing.
    CHECK(dn::liquidDarkenScale(10.0f, 0.0f, 0.5f) == Catch::Approx(1.0f));
}

TEST_CASE("an interior's fog takes over 25 yards in", "[sky][mfog]") {
    // 0x007a1150's sphere weight and 0x007f16f0's 0.04 a yard.
    CHECK(dn::wmoFogSphereWeight(5.0f, 10.0f, 30.0f) == Catch::Approx(1.0f));
    CHECK(dn::wmoFogSphereWeight(20.0f, 10.0f, 30.0f) == Catch::Approx(0.5f));
    CHECK(dn::wmoFogSphereWeight(40.0f, 10.0f, 30.0f) == Catch::Approx(0.0f));
    CHECK(dn::wmoFogBlend(0.0f) == Catch::Approx(0.0f));
    CHECK(dn::wmoFogBlend(12.5f) == Catch::Approx(0.5f));
    CHECK(dn::wmoFogBlend(100.0f) == Catch::Approx(1.0f));
}

TEST_CASE("a WMO's fog under liquid follows the LiquidType's flags", "[sky][mfog]") {
    using C = dn::WmoFogChoice;
    // Out of liquid it is the air fog, whatever the flags (0x007f16f0).
    CHECK(dn::wmoFogChoice(false, 0x160u, 0u) == C::Air);
    // In liquid, the liquid fog by default.
    CHECK(dn::wmoFogChoice(true, 0u, 0u) == C::Liquid);
    // LiquidType 0x20: only if the fog record has 0x100.
    CHECK(dn::wmoFogChoice(true, 0x20u, 0u) == C::None);
    CHECK(dn::wmoFogChoice(true, 0x20u, 0x100u) == C::Liquid);
    // LiquidType 0x100: not unless the fog record has 0x10.
    CHECK(dn::wmoFogChoice(true, 0x100u, 0u) == C::None);
    CHECK(dn::wmoFogChoice(true, 0x100u, 0x10u) == C::Liquid);
    CHECK(dn::wmoFogChoice(true, 0x120u, 0x10u) == C::None);
    CHECK(dn::wmoFogChoice(true, 0x120u, 0x110u) == C::Liquid);
    // LiquidType 0x40 makes the liquid fog the whole of it, no 25-yard blend.
    CHECK(dn::wmoLiquidFogIsWhole(C::Liquid, 0x40u));
    CHECK_FALSE(dn::wmoLiquidFogIsWhole(C::Liquid, 0u));
    CHECK_FALSE(dn::wmoLiquidFogIsWhole(C::Air, 0x40u));
    CHECK_FALSE(dn::wmoLiquidFogIsWhole(C::None, 0x40u));
}

TEST_CASE("the sky glows at dawn and dusk where HighlightSky says", "[sky][highlight]") {
    // 0xaf4b7c: none at 03:00, all at 06:30, none by 07:00; all at 21:30.
    CHECK(dn::skyHighlightStrength(3.0f / 24.0f, 1.0f) == Catch::Approx(0.0f));
    CHECK(dn::skyHighlightStrength(6.5f / 24.0f, 1.0f) == Catch::Approx(1.0f));
    CHECK(dn::skyHighlightStrength(7.0f / 24.0f, 1.0f) == Catch::Approx(0.0f));
    CHECK(dn::skyHighlightStrength(12.0f / 24.0f, 1.0f) == Catch::Approx(0.0f));
    CHECK(dn::skyHighlightStrength(21.5f / 24.0f, 1.0f) == Catch::Approx(1.0f));
    CHECK(dn::skyHighlightStrength(21.0f / 24.0f, 1.0f) == Catch::Approx(0.5f).margin(0.01f));
    // Nothing without the LightParams switch.
    CHECK(dn::skyHighlightStrength(6.5f / 24.0f, 0.0f) == Catch::Approx(0.0f));

    // The azimuth curve starts at atan2(y, x) of the camera's heading, as a
    // turn, plus a quarter (0x007f3920, 0x007f0530).
    CHECK(dn::skyHighlightPhase(glm::vec3(1.0f, 0.0f, 0.0f)) == Catch::Approx(0.25f));
    CHECK(dn::skyHighlightPhase(glm::vec3(0.0f, 1.0f, 0.0f)) == Catch::Approx(0.5f));
    CHECK(dn::skyHighlightPhase(glm::vec3(-1.0f, 0.0f, 0.0f)) == Catch::Approx(0.75f));
    CHECK(dn::skyHighlightPhase(glm::vec3(0.0f, -1.0f, 0.0f)) == Catch::Approx(1.0f));

    // A band at az 1 is itself; at az 0 it is pulled toward ch3 by the
    // strength; below 0 on toward ch2.
    const glm::vec3 band(1.0f, 0.0f, 0.0f), middle(0.0f, 1.0f, 0.0f), top(0.0f, 0.0f, 1.0f);
    const glm::vec3 same = dn::skyHighlightRow(band, middle, top, 1.0f, 1.0f);
    CHECK(same.r == Catch::Approx(1.0f));
    const glm::vec3 pulled = dn::skyHighlightRow(band, middle, top, 1.0f, 0.0f);
    CHECK(pulled.g == Catch::Approx(1.0f));
    CHECK(pulled.r == Catch::Approx(0.0f));
    const glm::vec3 lifted = dn::skyHighlightRow(band, middle, top, 1.0f, -0.7f);
    // ch3, then 0.7 of the way to ch2, 0.7 of that: 0.49 of ch2.
    CHECK(lifted.b == Catch::Approx(0.49f));
    CHECK(lifted.g == Catch::Approx(0.51f));
    // No strength, no glow.
    const glm::vec3 none = dn::skyHighlightRow(band, middle, top, 0.0f, -0.7f);
    CHECK(none.r == Catch::Approx(1.0f));
}

TEST_CASE("the stars are out from 22:30 to 04:30", "[sky][stars]") {
    // 0xaf4c20 through 0x007ee0d0: the curve times 254, plus one, truncated.
    CHECK(dn::starsAlphaByte(0.0f) == 255);
    CHECK(dn::starsAlphaByte(2.0f / 24.0f) == 255);
    CHECK(dn::starsAlphaByte(4.5f / 24.0f) == 1);
    CHECK(dn::starsAlphaByte(0.5f) == 1);
    CHECK(dn::starsAlphaByte(23.25f / 24.0f) == 128);
    // 0x009abd50 draws them only above 1.
    CHECK(dn::starsAlpha(0.5f) == 0.0f);
    CHECK(dn::starsAlpha(0.0f) == Catch::Approx(1.0f));
    CHECK(std::string(dn::kStarsModelPath) == "Environments\\Stars\\stars.mdl");
}

TEST_CASE("a sky model hides the procedural sky only without flag 0x2", "[sky][skybox]") {
    using Layer = LightingManager::SkyboxLayer;
    // 0x007f09b0: past 0.99 and not 'combine with the procedural sky'.
    CHECK(LightingManager::skyboxHidesProceduralSky({Layer{.path = "a", .weight = 1.0f, .flags = 0}}));
    CHECK_FALSE(LightingManager::skyboxHidesProceduralSky(
        {Layer{.path = "a", .weight = 1.0f, .flags = LightingManager::kSkyboxFlagCombineProcedural}}));
    // Half way up hides nothing: it used to, at 0.5 coverage.
    CHECK_FALSE(LightingManager::skyboxHidesProceduralSky({Layer{.path = "a", .weight = 0.6f, .flags = 0}}));
    // The death model hides it whatever its flags.
    CHECK(LightingManager::skyboxHidesProceduralSky(
        {Layer{.path = "d", .weight = 1.0f, .flags = LightingManager::kSkyboxFlagCombineProcedural,
               .deathOverride = true}}));
    // The glare fades by the heaviest model, or by the death model alone.
    CHECK(LightingManager::skyboxGlareWeight({Layer{.path = "a", .weight = 0.3f},
                                              Layer{.path = "b", .weight = 0.7f}}) ==
          Catch::Approx(0.7f));
    CHECK(LightingManager::skyboxGlareWeight({Layer{.path = "a", .weight = 0.9f},
                                              Layer{.path = "d", .weight = 0.2f,
                                                    .deathOverride = true}}) ==
          Catch::Approx(0.2f));
}

TEST_CASE("the new light channels blend with the rest", "[sky][lerp]") {
    LightingParams a;
    LightingParams b;
    a.cloudSunColor = glm::vec3(0.0f);
    b.cloudSunColor = glm::vec3(1.0f);
    a.oceanFarColor = glm::vec3(0.0f);
    b.oceanFarColor = glm::vec3(1.0f);
    a.glow = 0.0f;
    b.glow = 1.0f;
    a.shadowOpacity = 0.0f;
    b.shadowOpacity = 1.0f;
    const LightingParams m = wowee::rendering::lerpLightingParams(a, b, 0.25f);
    CHECK(m.cloudSunColor.r == Catch::Approx(0.25f));
    CHECK(m.oceanFarColor.g == Catch::Approx(0.25f));
    CHECK(m.glow == Catch::Approx(0.25f));
    CHECK(m.shadowOpacity == Catch::Approx(0.25f));
}

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putF(std::vector<uint8_t>& out, float f) {
    uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    put32(out, v);
}

}  // namespace

TEST_CASE("MFOG is read into the model's fogs", "[wmo][mfog]") {
    std::vector<uint8_t> wmo;
    put32(wmo, 0x4D564552);  // MVER
    put32(wmo, 4);
    put32(wmo, 17);
    put32(wmo, 0x4D464F47);  // MFOG
    put32(wmo, 48);
    put32(wmo, 0x1);                 // flags
    putF(wmo, 1.0f); putF(wmo, 2.0f); putF(wmo, 3.0f);
    putF(wmo, 10.0f); putF(wmo, 30.0f);
    putF(wmo, 120.0f); putF(wmo, 0.25f);
    put32(wmo, 0xFF102030);          // BGRA: r 0x10, g 0x20, b 0x30
    putF(wmo, 50.0f); putF(wmo, 0.5f);
    put32(wmo, 0xFF000080);

    const auto model = wowee::pipeline::WMOLoader::load(wmo);
    REQUIRE(model.fogs.size() == 1);
    const auto& f = model.fogs[0];
    CHECK(f.flags == 0x1u);
    CHECK(f.position.z == Catch::Approx(3.0f));
    CHECK(f.smallRadius == Catch::Approx(10.0f));
    CHECK(f.largeRadius == Catch::Approx(30.0f));
    CHECK(f.endDist == Catch::Approx(120.0f));
    CHECK(f.startFactor == Catch::Approx(0.25f));
    CHECK(f.color1.r == Catch::Approx(0x10 / 255.0f));
    CHECK(f.color1.b == Catch::Approx(0x30 / 255.0f));
    CHECK(f.endDist2 == Catch::Approx(50.0f));
    CHECK(f.color2.b == Catch::Approx(0x80 / 255.0f));
}

TEST_CASE("MODR, the transition batch count and the MOHD flags are read", "[wmo][modr]") {
    std::vector<uint8_t> root;
    put32(root, 0x4D564552);  // MVER
    put32(root, 4);
    put32(root, 17);
    put32(root, 0x4D4F4844);  // MOHD
    put32(root, 64);
    for (int i = 0; i < 7; ++i) put32(root, 0);  // counts
    put32(root, 0x00102030);                     // ambient
    put32(root, 0);                              // wmo id
    for (int i = 0; i < 6; ++i) putF(root, 0.0f);
    put32(root, 0x00010002);                     // flags 0x2, numLod 1
    auto model = wowee::pipeline::WMOLoader::load(root);
    CHECK(model.flags == 0x2u);

    std::vector<uint8_t> group;
    put32(group, 0x4D564552);  // MVER
    put32(group, 4);
    put32(group, 17);
    put32(group, 0x4D4F4750);  // MOGP
    put32(group, 68 + 12 + 4);
    put32(group, 0);
    put32(group, 0);
    put32(group, 0x2000);      // flags
    for (int i = 0; i < 7; ++i) put32(group, 0);   // bounds, portals
    put32(group, 0x00050002);  // 2 transition batches, 5 interior ones
    for (int i = 0; i < 6; ++i) put32(group, 0);   // rest of the header
    put32(group, 0x4D4F4452);  // MODR
    put32(group, 4);
    put32(group, 0x00090003);  // refs 3 and 9
    put32(group, 0);           // padding after it
    model.groups.resize(1);
    // No geometry, so the loader reports the group as empty; the refs are
    // read all the same.
    (void)wowee::pipeline::WMOLoader::loadGroup(group, model, 0);
    CHECK(model.groups[0].flags == 0x2000u);
    // The transition batches' count, whose vertices keep their alpha
    // (0x007d7380 reads it at the group's +0x5c).
    CHECK(model.groups[0].transBatchCount == 2);
    REQUIRE(model.groups[0].doodadRefs.size() == 2);
    CHECK(model.groups[0].doodadRefs[0] == 3);
    CHECK(model.groups[0].doodadRefs[1] == 9);
}

TEST_CASE("MCSH's bits are read low bit first, edges fixed", "[adt][mcsh]") {
    std::vector<uint8_t> bits(512, 0);
    bits[0] = 0x05;        // texels 0 and 2 of row 0
    bits[62 * 8 + 7] = 0x40;  // texel 62 of row 62
    wowee::pipeline::MapChunk chunk;
    wowee::pipeline::ADTLoader::parseMCSH(bits, true, chunk);
    REQUIRE(chunk.shadowMap.size() == 64u * 64u);
    CHECK(chunk.shadowMap[0] == 1);
    CHECK(chunk.shadowMap[1] == 0);
    CHECK(chunk.shadowMap[2] == 1);
    CHECK(chunk.shadowMap[62 * 64 + 62] == 1);
    // The last column and row repeat the ones before them.
    CHECK(chunk.shadowMap[62 * 64 + 63] == 1);
    CHECK(chunk.shadowMap[63 * 64 + 62] == 1);
}
