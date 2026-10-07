// A WMO material's window light (rendering/wmo_sidn.hpp): the level curve,
// frameSidnColor and the c29 the MapObj programs add.

#include <catch_amalgamated.hpp>

#include "rendering/wmo_sidn.hpp"

namespace ws = wowee::rendering::wmo_sidn;

TEST_CASE("windows are lit at night by the curve at 0xaf4c80", "[wmo]") {
    CHECK(ws::windowLevel(0.0f) == 1.0f);   // midnight
    CHECK(ws::windowLevel(0.25f) == 1.0f);  // 06:00
    CHECK(ws::windowLevel(0.5f) == 0.0f);   // noon
    CHECK(ws::windowLevel(0.27083333f) == Catch::Approx(0.5f).margin(1e-4));  // 06:30
    CHECK(ws::windowLevel(0.875f) == Catch::Approx(0.5f).margin(1e-4));       // 21:00
    CHECK(ws::windowLevel(0.95f) == 1.0f);
}

TEST_CASE("frameSidnColor scales each byte by the level", "[wmo]") {
    // Full level is 254 (round(254.5) to even): 0xff x 254 >> 8 = 0xfd.
    CHECK(ws::frameSidnColor(0xffffffffu, 1.0f) == 0x00fdfdfdu);
    CHECK(ws::frameSidnColor(0x80402010u, 0.0f) == 0u);
    // Half: round(127.0) = 127.
    CHECK(ws::frameSidnColor(0x00ff8000u, 0.5f) == ((0xffu * 127 >> 8) << 16 | (0x80u * 127 >> 8) << 8));
}

TEST_CASE("c29 is the saturated sum halved, alpha kept", "[wmo]") {
    CHECK(ws::emissive(0, 0x00fdfdfdu) == 0x007e7e7eu);
    CHECK(ws::emissive(0x00808080u, 0x00808080u) == 0x007f7f7fu);  // saturates at 0xff
    CHECK(ws::emissive(0xff000000u, 0x00102030u) == 0xff081018u);
}
