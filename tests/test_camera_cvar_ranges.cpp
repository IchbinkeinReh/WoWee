// The camera CVars' ranges (0x005fd630-0x005fd8c0): both ends allowed, a
// value outside refused, a CVar without a range never refused.
#include <catch_amalgamated.hpp>

#include "addons/camera_cvar_ranges.hpp"

using wowee::addons::cameraCVarAccepts;
using wowee::addons::cameraCVarRange;

TEST_CASE("camera CVars accept only their registered range", "[cvar][camera]") {
    CHECK(cameraCVarAccepts("cameraview", "0"));
    CHECK(cameraCVarAccepts("cameraview", "7"));
    CHECK_FALSE(cameraCVarAccepts("cameraview", "8"));
    CHECK_FALSE(cameraCVarAccepts("cameraview", "-1"));

    CHECK(cameraCVarAccepts("cameradistancemax", "50"));
    CHECK_FALSE(cameraCVarAccepts("cameradistancemax", "50.5"));

    CHECK(cameraCVarAccepts("camerasurfacepitch", "-89"));
    CHECK(cameraCVarAccepts("camerapitchsmoothmax", "89"));
    CHECK_FALSE(cameraCVarAccepts("camerapitchsmoothmin", "90"));

    CHECK_FALSE(cameraCVarAccepts("camerasmoothtimemin", "0"));
    CHECK(cameraCVarAccepts("camerasmoothtimemin", "0.001"));
    CHECK(cameraCVarAccepts("camerayawsmoothmax", "360"));
    CHECK_FALSE(cameraCVarAccepts("camerapitchmovespeed", "0"));
    CHECK(cameraCVarAccepts("camerapitchmovespeed", "0.1"));
    CHECK(cameraCVarAccepts("cameragroundsmoothspeed", "360"));
    CHECK_FALSE(cameraCVarAccepts("cameradistancesmoothspeed", "51"));
    CHECK(cameraCVarAccepts("camerasmoothstyle", "5"));
    CHECK_FALSE(cameraCVarAccepts("camerasmoothtrackingstyle", "6"));
}

TEST_CASE("a CVar without a range takes anything", "[cvar][camera]") {
    CHECK_FALSE(cameraCVarRange("cameraviewblendstyle").has_value());
    CHECK(cameraCVarAccepts("camerabobbing", "1000"));
    CHECK(cameraCVarAccepts("uiscale", "-3"));
}
