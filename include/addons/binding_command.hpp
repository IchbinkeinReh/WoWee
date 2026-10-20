#pragma once

#include <cstring>
#include <string>

namespace wowee::addons {

/// A binding command the key dispatch carries out itself rather than through a
/// script from Bindings.xml (0x00563150): SPELL, ITEM, MACRO and CLICK, the
/// four that SetBindingSpell, SetBindingItem, SetBindingMacro and
/// SetBindingClick write.
struct DirectBinding {
    enum class Kind { None, Spell, Item, Macro, Click };
    Kind kind = Kind::None;
    /// The spell, item or macro named, or the button to click.
    std::string name;
    /// CLICK only: the mouse button the click is made with.
    std::string mouseButton;
    /// MACRO only: the name was all digits, so it is the macro's index
    /// (0x00563150 reads it with 0x0076f0d0 and asks 0x00566a70 for index-1).
    bool macroByIndex = false;
    /// CLICK only: the command names a mouse button after a ':'.
    bool clickable = false;
};

/// What the command says, matched by its prefix the way 0x00563150 matches it:
/// case-insensitively, the space included. A CLICK without a colon after the
/// button's name is still a CLICK - the client claims it and runs no script -
/// but clicks nothing, finding no ':' to split at; clickable says which.
inline DirectBinding parseDirectBinding(const std::string& command) {
    auto hasPrefix = [&](const char* prefix) {
        const size_t n = std::strlen(prefix);
        if (command.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            const char a = command[i];
            const char up = (a >= 'a' && a <= 'z') ? static_cast<char>(a - 'a' + 'A') : a;
            if (up != prefix[i]) return false;
        }
        return true;
    };
    DirectBinding out;
    if (hasPrefix("SPELL ")) {
        out.kind = DirectBinding::Kind::Spell;
        out.name = command.substr(6);
    } else if (hasPrefix("ITEM ")) {
        out.kind = DirectBinding::Kind::Item;
        out.name = command.substr(5);
    } else if (hasPrefix("MACRO ")) {
        out.kind = DirectBinding::Kind::Macro;
        out.name = command.substr(6);
        bool digits = !out.name.empty();
        for (char c : out.name) {
            if (c < '0' || c > '9') { digits = false; break; }
        }
        out.macroByIndex = digits;
    } else if (hasPrefix("CLICK ")) {
        out.kind = DirectBinding::Kind::Click;
        const std::string rest = command.substr(6);
        const size_t colon = rest.find(':');
        out.name = rest.substr(0, colon);
        if (colon != std::string::npos) {
            out.clickable = true;
            out.mouseButton = rest.substr(colon + 1);
        }
    }
    return out;
}

}  // namespace wowee::addons
