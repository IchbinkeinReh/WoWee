// IsObjectType's chains (include/addons/object_type.hpp) against the client's
// class tests (0x00482b10 - 0x0096de80).
#include <catch_amalgamated.hpp>

#include "addons/object_type.hpp"

#include <cctype>
#include <string>

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

TEST_CASE("CreateFrame's type is the class's own spelling, in whatever case it was asked",
          "[object_type][createframe]") {
    using wowee::addons::creatableFrameType;
    using wowee::addons::kCreatableFrameTypes;
    for (const char* t : kCreatableFrameTypes) {
        std::string upper(t), lower(t);
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CAPTURE(t);
        CHECK(std::string(creatableFrameType(t)) == t);
        CHECK(std::string(creatableFrameType(upper.c_str())) == t);
        CHECK(std::string(creatableFrameType(lower.c_str())) == t);
    }
    CHECK(creatableFrameType("Texture") == nullptr);
    CHECK(creatableFrameType("FontString") == nullptr);
    CHECK(creatableFrameType("Buttons") == nullptr);
    CHECK(creatableFrameType("") == nullptr);
    CHECK(creatableFrameType(nullptr) == nullptr);
}

TEST_CASE("a button made as FrameXML spells it takes a click", "[object_type][createframe]") {
    // The fault this guards: OptionsList_OnLoad makes its category buttons
    // with CreateFrame("BUTTON", ...). The type was kept as given, the script
    // check compares it with "Button", and OptionsListButtonTemplate's
    // OnClick was refused - every category in the Video, Sound and Interface
    // options lit up under the pointer and opened nothing.
    using wowee::addons::creatableFrameType;
    using wowee::addons::frameTypeHasScript;
    using wowee::addons::frameTypeScriptName;
    for (const char* asked : {"BUTTON", "button", "Button"}) {
        CAPTURE(asked);
        const std::string type = creatableFrameType(asked);
        CHECK(frameTypeHasScript(type, "OnClick"));
        CHECK(std::string(frameTypeScriptName(type, "onclick")) == "OnClick");
        CHECK(frameTypeHasScript(type, "PreClick"));
    }
    CHECK(frameTypeHasScript(creatableFrameType("CHECKBUTTON"), "OnClick"));
    CHECK(frameTypeHasScript(creatableFrameType("SLIDER"), "OnValueChanged"));
    CHECK(frameTypeHasScript(creatableFrameType("EDITBOX"), "OnEnterPressed"));
    CHECK(frameTypeHasScript(creatableFrameType("SCROLLFRAME"), "OnVerticalScroll"));
    CHECK(frameTypeHasScript(creatableFrameType("FRAME"), "OnShow"));
    CHECK_FALSE(frameTypeHasScript(creatableFrameType("FRAME"), "OnClick"));
}
