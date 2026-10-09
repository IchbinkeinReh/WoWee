// The interface's art prepared for additive and desaturated drawing
// (ui/texture_variants.hpp; Desaturate.bls, 0x00483060).
#include <catch_amalgamated.hpp>

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

TEST_CASE("additive: alpha from brightness", "[texture_variants]") {
    std::vector<uint8_t> px = {0, 0, 0, 255, 255, 10, 10, 255, 128, 0, 0, 128};
    prepareAdditive(px);
    CHECK(px[3] == 0);
    CHECK(px[7] == 255);
    CHECK(px[11] == 64);
}
