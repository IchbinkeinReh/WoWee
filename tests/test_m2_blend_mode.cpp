// Whether an M2 batch is alpha tested, which decides the pipeline it draws on.
//
// A batch whose texture carries no alpha channel was forced onto the cutout
// pipeline whenever its blend mode was 2 or higher. For alpha blend that is
// reasonable - blending by an alpha that is 1 everywhere draws solid, so
// cutting out at least keeps the artist's silhouette.
//
// For the additive modes it destroys the effect. Additive does not use alpha
// for transparency: black is what disappears, because adding zero changes
// nothing. Glow cards are authored exactly that way - an opaque texture,
// bright in the middle and black at the edges.
//
// The oracle is the model and the texture Orgrimmar's bonfire ships.
// orcpvpbonfirelarge material 0 is blend mode 4, and its texture
// GENERICGLOW_ALPHA_128.BLP reports alphaDepth 0 in its own header - named for
// an alpha it does not have. Forced to cutout, every texel passed the test and
// the flame rendered as a flat opaque grey disc behind the logs.
#include <catch_amalgamated.hpp>

#include "rendering/m2_blend_mode.hpp"

using wowee::rendering::m2BatchNeedsAlphaTest;
using wowee::rendering::m2BlendIsAdditive;
using wowee::rendering::m2BlendIsModulate;

TEST_CASE("the Orgrimmar bonfire's glow card is not alpha tested", "[m2]") {
    // Blend mode 4, no alpha channel: the case that showed it.
    CHECK_FALSE(m2BatchNeedsAlphaTest(4, false));
    // And it stays untested if the texture does happen to carry alpha.
    CHECK_FALSE(m2BatchNeedsAlphaTest(4, true));
}

TEST_CASE("no additive batch is alpha tested", "[m2]") {
    // Black is the transparent colour for these, whatever the texture holds.
    for (uint8_t mode : {uint8_t{3}, uint8_t{4}}) {
        INFO("blend mode " << int(mode));
        CHECK(m2BlendIsAdditive(mode));
        CHECK_FALSE(m2BatchNeedsAlphaTest(mode, false));
        CHECK_FALSE(m2BatchNeedsAlphaTest(mode, true));
    }
}

TEST_CASE("alpha key is tested, whatever the texture", "[m2]") {
    // The 3.3.5a client sets the alpha reference from the blend mode alone
    // (FUN_0081fe90). An alpha-key texture with no alpha passes the test
    // everywhere, so testing it costs nothing and decides nothing.
    CHECK(m2BatchNeedsAlphaTest(1, true));
    CHECK(m2BatchNeedsAlphaTest(1, false));
}

TEST_CASE("a blended batch is blended, not cut out", "[m2]") {
    // The client blends a mode-2 batch whatever its texture: without alpha
    // that is an alpha of 1, which is opaque, and it does not cut it out.
    CHECK_FALSE(m2BatchNeedsAlphaTest(2, false));
    CHECK_FALSE(m2BatchNeedsAlphaTest(2, true));
}

TEST_CASE("opaque is never tested", "[m2]") {
    CHECK_FALSE(m2BatchNeedsAlphaTest(0, true));
    CHECK_FALSE(m2BatchNeedsAlphaTest(0, false));
    CHECK_FALSE(m2BlendIsAdditive(0));
    CHECK_FALSE(m2BlendIsAdditive(1));
    CHECK_FALSE(m2BlendIsAdditive(2));
}

TEST_CASE("the modulate modes are never alpha tested", "[m2]") {
    // The 3.3.5a client draws 5 as Mod (DST_COLOR, ZERO) and 6 as Mod2x
    // (DST_COLOR, SRC_COLOR): factors that never read alpha, so there is
    // nothing missing for a cutout to stand in for. Forced onto the cutout
    // pipeline a tint layer is drawn as a solid sheet.
    CHECK(m2BlendIsModulate(5));
    CHECK(m2BlendIsModulate(6));
    CHECK_FALSE(m2BatchNeedsAlphaTest(5, false));
    CHECK_FALSE(m2BatchNeedsAlphaTest(6, false));
    CHECK_FALSE(m2BatchNeedsAlphaTest(5, true));
    CHECK_FALSE(m2BatchNeedsAlphaTest(6, true));
}

TEST_CASE("blend mode 7 is plain alpha", "[m2]") {
    // The client's M2 -> Gx table maps 7 to Alpha, not to an additive blend.
    CHECK_FALSE(m2BlendIsAdditive(7));
    CHECK_FALSE(m2BlendIsModulate(7));
    CHECK_FALSE(m2BatchNeedsAlphaTest(7, false));
    CHECK_FALSE(m2BatchNeedsAlphaTest(7, true));
}
