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
#include <initializer_list>
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


/// The frame types CreateFrame makes (0x0081bb20's factory list), each in the
/// class's own spelling.
inline constexpr const char* kCreatableFrameTypes[] = {
    "Frame", "Button", "CheckButton", "ColorSelect", "EditBox", "MessageFrame", "Model",
    "MovieFrame", "ScrollFrame", "ScrollingMessageFrame", "SimpleHTML", "Slider", "StatusBar",
    "WorldFrame", "GameTooltip", "Cooldown", "Minimap", "PlayerModel", "DressUpModel",
    "TabardModel", "QuestPOIFrame"};

/// The type CreateFrame makes for the one it was asked for: looked up without
/// regard to case, as the client's factory does, and answered in the class's
/// own spelling - or nullptr for a type it cannot make.
///
/// The spelling matters because the frame is the class, not the string:
/// FrameXML asks for "BUTTON" (OptionsList_OnLoad, HybridScrollFrame's
/// buttons, the chat config's tabs) and "FRAME", and a Button that kept
/// "BUTTON" as its type was no Button to anything comparing the type - its
/// template's OnClick was refused as a script that kind of frame does not
/// have, and every options category in the game menu's windows came out
/// unclickable.
inline const char* creatableFrameType(const char* asked) {
    if (!asked) return nullptr;
    for (const char* t : kCreatableFrameTypes)
        if (sameTypeName(asked, t)) return t;
    return nullptr;
}

/// The handler's own spelling when this kind of frame has it, matched in any
/// case as the client's lookups do (0x0076e780), or nullptr.
inline const char* frameTypeScriptName(const std::string& type, const std::string& handler) {
    using List = std::initializer_list<const char*>;
    static const List kFrame = {
        "OnEvent", "OnLoad", "OnSizeChanged", "OnUpdate", "OnShow", "OnHide",
        "OnEnter", "OnLeave", "OnMouseDown", "OnMouseUp", "OnMouseWheel",
        "OnDragStart", "OnDragStop", "OnReceiveDrag", "OnChar", "OnKeyDown",
        "OnKeyUp", "OnAttributeChanged", "OnEnable", "OnDisable"};
    static const List kButton = {"PreClick", "OnClick", "PostClick", "OnDoubleClick"};   // 0x0096f200
    static const List kEditBox = {                                                        // 0x009628f0
        "OnEnterPressed", "OnEscapePressed", "OnSpacePressed", "OnTabPressed",
        "OnTextChanged", "OnTextSet", "OnCursorChanged", "OnInputLanguageChanged",
        "OnEditFocusGained", "OnEditFocusLost", "OnCharComposition"};
    static const List kValue = {"OnValueChanged", "OnMinMaxChanged"};                     // 0x00961630, 0x0096bca0
    static const List kScroll = {"OnHorizontalScroll", "OnVerticalScroll", "OnScrollRangeChanged"};  // 0x0096b0b0
    static const List kTooltip = {                                                        // 0x0061a280
        "OnTooltipSetDefaultAnchor", "OnTooltipCleared", "OnTooltipAddMoney",
        "OnTooltipSetUnit", "OnTooltipSetItem", "OnTooltipSetSpell", "OnTooltipSetQuest",
        "OnTooltipSetAchievement", "OnTooltipSetEquipmentSet", "OnTooltipSetFrameStack"};
    static const List kModel = {"OnUpdateModel", "OnAnimFinished"};                       // 0x0095f420
    static const List kMovie = {"OnMovieFinished", "OnMovieShowSubtitle", "OnMovieHideSubtitle"};  // 0x0095db80
    static const List kHyperlink = {"OnHyperlinkEnter", "OnHyperlinkLeave", "OnHyperlinkClick"};  // 0x00978610
    static const List kColor = {"OnColorSelect"};                                         // 0x0096dc50
    static const List kScrollMsg = {"OnMessageScrollChanged"};                            // 0x00969040
    auto find = [&](const List& list) -> const char* {
        for (const char* n : list)
            if (sameTypeName(n, handler.c_str())) return n;
        return nullptr;
    };
    if (const char* n = find(kFrame)) return n;
    if (type == "Button" || type == "CheckButton") return find(kButton);
    if (type == "EditBox") return find(kEditBox);
    if (type == "Slider" || type == "StatusBar") return find(kValue);
    if (type == "ScrollFrame") return find(kScroll);
    if (type == "GameTooltip") return find(kTooltip);
    if (type == "ColorSelect") return find(kColor);
    if (type == "MovieFrame") return find(kMovie);
    if (type == "Model" || type == "PlayerModel" || type == "DressUpModel" ||
        type == "TabardModel" || type == "CinematicModel")
        return find(kModel);
    if (type == "ScrollingMessageFrame") {
        if (const char* n = find(kScrollMsg)) return n;
        return find(kHyperlink);
    }
    if (type == "MessageFrame" || type == "SimpleHTML") return find(kHyperlink);
    return nullptr;
}

/// HasScript("handler") (0x0049eab0): whether this kind of frame has the
/// handler at all, set or not - the class's own name lookup (vtable slot 0xc),
/// each falling back to its parent's: the frame's (0x0048e680, OnEvent from
/// 0x00816830) under every one.
inline bool frameTypeHasScript(const std::string& type, const std::string& handler) {
    return frameTypeScriptName(type, handler) != nullptr;
}

}  // namespace wowee::addons
