// The client's own CVars and the flags it registers them with (0x00767fc0),
// which GetCVarInfo and GetCVarAbsoluteMin/Max answer from.
#include <catch_amalgamated.hpp>

#include "addons/client_cvar_registry.hpp"

using wowee::addons::clientCVarFlags;
using wowee::addons::kCVarHidden;
using wowee::addons::kCVarStoredWithAccount;
using wowee::addons::kCVarStoredWithCharacter;

TEST_CASE("a client CVar answers its registration flags", "[client_cvar_registry]") {
    REQUIRE(clientCVarFlags("autoSelfCast"));
    CHECK(*clientCVarFlags("autoSelfCast") == kCVarStoredWithCharacter);
    REQUIRE(clientCVarFlags("rotateMinimap"));
    CHECK(*clientCVarFlags("rotateMinimap") == kCVarStoredWithAccount);
    // trackedQuests is kept per character, and 0x100 besides.
    REQUIRE(clientCVarFlags("trackedQuests"));
    CHECK((*clientCVarFlags("trackedQuests") & kCVarStoredWithCharacter) != 0);
    // A plain one: no storage bits at all.
    REQUIRE(clientCVarFlags("movie"));
    CHECK(*clientCVarFlags("movie") == 0);
}

TEST_CASE("names match whatever their case", "[client_cvar_registry]") {
    CHECK(clientCVarFlags("AUTOSELFCAST") == clientCVarFlags("autoSelfCast"));
    CHECK(clientCVarFlags("uiscale").has_value());
}

TEST_CASE("the camera's loop-built CVars are there", "[client_cvar_registry]") {
    // The eight views' distance, pitch and yaw are hidden.
    REQUIRE(clientCVarFlags("cameraDistanceD"));
    CHECK((*clientCVarFlags("cameraDistanceD") & kCVarHidden) != 0);
    REQUIRE(clientCVarFlags("cameraYawBarber Shop"));
    CHECK((*clientCVarFlags("cameraYawBarber Shop") & kCVarHidden) != 0);
    // Smoothing and tilt, five styles each.
    CHECK(clientCVarFlags("cameraSmoothSmarterTurnFactor") == kCVarStoredWithAccount);
    CHECK(clientCVarFlags("cameraSmoothViewDataNeverPitchDelay") == kCVarStoredWithAccount);
    CHECK(clientCVarFlags("cameraTerrainTiltSplineSwimAbsorb") == kCVarStoredWithAccount);
    CHECK_FALSE(clientCVarFlags("cameraSmoothSmarterTurn").has_value());
    CHECK_FALSE(clientCVarFlags("cameraDistanceF").has_value());
}

TEST_CASE("a name the client never registers is not there", "[client_cvar_registry]") {
    CHECK_FALSE(clientCVarFlags("notACVar").has_value());
    CHECK_FALSE(clientCVarFlags("").has_value());
}
