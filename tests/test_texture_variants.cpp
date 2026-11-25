// The interface's art prepared for desaturated drawing, and the blend modes' names
// (ui/texture_variants.hpp; Desaturate.bls, 0x00483060).
#include <catch_amalgamated.hpp>

#include "ui/texture_blend.hpp"
#include "ui/texture_variants.hpp"

using namespace wowee::ui;

TEST_CASE("desaturated: the luminance in all three channels, alpha kept", "[texture_variants]") {
    std::vector<uint8_t> px = {255, 0, 0, 200, 0, 255, 0, 10, 0, 0, 255, 255, 255, 255, 255, 0};
    prepareDesaturated(px);
    CHECK(px[0] == 76);
    CHECK(px[1] == 76);
    CHECK(px[2] == 76);
    CHECK(px[3] == 200);
    CHECK(px[4] == 150);
    CHECK(px[7] == 10);
    CHECK(px[8] == 29);
    CHECK(px[12] == 255);
    CHECK(px[15] == 0);
}

// 0x00814ff0, 0x0048c040
TEST_CASE("the blend modes by their names", "[texture_variants]") {
    CHECK(textureBlendOf("ADD") == TextureBlend::Add);
    CHECK(textureBlendOf("add") == TextureBlend::Add);
    CHECK(textureBlendOf("Mod") == TextureBlend::Mod);
    CHECK(textureBlendOf("DISABLE") == TextureBlend::Disable);
    CHECK(textureBlendOf("ALPHAKEY") == TextureBlend::AlphaKey);
    CHECK(textureBlendOf("BLEND") == TextureBlend::Blend);
    CHECK_FALSE(textureBlendOf("ADDS"));
    CHECK_FALSE(textureBlendOf(""));
    CHECK(static_cast<int>(TextureBlend::Add) == 3);
    for (const char* name : {"DISABLE", "ALPHAKEY", "BLEND", "ADD", "MOD"})
        CHECK(std::string(textureBlendName(*textureBlendOf(name))) == name);
}
