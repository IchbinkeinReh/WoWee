// IsObjectType's chains (include/addons/object_type.hpp) against the client's
// class tests (0x00482b10 - 0x0096de80).
#include <catch_amalgamated.hpp>

#include "addons/object_type.hpp"

using wowee::addons::objectTypeMatches;

TEST_CASE("a type answers to itself and its parents, in any case", "[object_type]") {
    CHECK(objectTypeMatches("CheckButton", "CheckButton"));
    CHECK(objectTypeMatches("CheckButton", "button"));
    CHECK(objectTypeMatches("CheckButton", "Frame"));
    CHECK(objectTypeMatches("CheckButton", "REGION"));
    CHECK(objectTypeMatches("CheckButton", "Object"));
    CHECK(objectTypeMatches("TabardModel", "PlayerModel"));
    CHECK(objectTypeMatches("TabardModel", "Model"));
    CHECK(objectTypeMatches("Texture", "Region"));
    CHECK(objectTypeMatches("FontString", "Object"));
    CHECK(objectTypeMatches("Slider", "Frame"));
}

TEST_CASE("a type does not answer to what it is not", "[object_type]") {
    // The fault this guards: the answer turned round, so every frame said it
    // was a Button and every texture a FontString.
    CHECK_FALSE(objectTypeMatches("Frame", "Button"));
    CHECK_FALSE(objectTypeMatches("Button", "CheckButton"));
    CHECK_FALSE(objectTypeMatches("Texture", "FontString"));
    CHECK_FALSE(objectTypeMatches("Texture", "Frame"));
    CHECK_FALSE(objectTypeMatches("ScrollingMessageFrame", "MessageFrame"));
    CHECK_FALSE(objectTypeMatches("Texture", "LayeredRegion"));
    CHECK_FALSE(objectTypeMatches("FontString", "LayeredRegion"));
    CHECK_FALSE(objectTypeMatches("Frame", ""));
}
