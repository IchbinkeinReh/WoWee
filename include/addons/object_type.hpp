#pragma once

// IsObjectType's answer: whether a widget of one type is also of another.
//
// WoW's IsObjectType is true for the type itself and everything it derives
// from - a CheckButton is a Button is a Frame is a Region is an Object - and
// FrameXML relies on that: it asks whether something is a Region to decide it
// can be positioned at all. Each class's test (0x00482c90 Texture, 0x004853c0
// FontString, 0x004926f0 Frame, 0x009622a0 CheckButton, 0x00597310
// PlayerModel, ...) compares without regard to case (0x0076e780) and then asks
// its parent class. ScrollingMessageFrame is not a MessageFrame, and there is
// no "LayeredRegion" in 3.3.5's chains, so a texture is not one.

#include <cctype>
#include <string>
#include <utility>

namespace wowee::addons {

/// strnicmp as the client's type tests use it: the same name in any case.
inline bool sameTypeName(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    return *a == *b;
}

/// The class a type derives from; nullptr for Object, Frame for any frame
/// kind not listed (Minimap, Cooldown, GameTooltip, StatusBar, EditBox, ...).
inline const char* parentObjectType(const std::string& type) {
    static const std::pair<const char*, const char*> kParents[] = {
        {"Region", "Object"},       {"Texture", "Region"},
        {"FontString", "Region"},   {"Frame", "Region"},
        {"Button", "Frame"},        {"CheckButton", "Button"},
        {"Model", "Frame"},         {"PlayerModel", "Model"},
        {"DressUpModel", "PlayerModel"}, {"TabardModel", "PlayerModel"},
    };
    if (sameTypeName(type.c_str(), "Object")) return nullptr;
    for (const auto& [child, parent] : kParents)
        if (sameTypeName(type.c_str(), child)) return parent;
    return "Frame";
}

inline bool objectTypeMatches(const std::string& actual, const std::string& asked) {
    for (std::string t = actual;;) {
        if (sameTypeName(t.c_str(), asked.c_str())) return true;
        const char* parent = parentObjectType(t);
        if (!parent) return false;
        t = parent;
    }
}

}  // namespace wowee::addons
