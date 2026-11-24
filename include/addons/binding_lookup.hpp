#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Which key binding a press runs.
namespace wowee::addons {

/// The keys a press is looked up as, in order (0x005622e0): with its modifiers
/// as held, then with fewer of them down to none - so SHIFT-W is W's where
/// SHIFT-W has nothing. A key of "-" is no modifier's end.
inline std::vector<std::string> bindingLookups(const std::string& key) {
    std::vector<std::string> modifiers;
    std::string rest = key;
    while (modifiers.size() < 3) {
        const size_t dash = rest.find('-');
        if (dash == std::string::npos || dash == 0) break;
        modifiers.push_back(rest.substr(0, dash + 1));
        rest = rest.substr(dash + 1);
    }
    static constexpr std::array<std::array<unsigned, 8>, 4> kMasks{{
        {0}, {1, 0}, {3, 2, 1, 0}, {7, 6, 5, 3, 4, 2, 1, 0}}};
    std::vector<std::string> out;
    for (size_t i = 0; i < (size_t{1} << modifiers.size()); ++i) {
        std::string candidate;
        for (size_t m = 0; m < modifiers.size(); ++m)
            if ((kMasks[modifiers.size()][i] & (1u << m)) != 0) candidate += modifiers[m];
        out.push_back(candidate + rest);
    }
    return out;
}

/// The modifiers as 0x00512d20 numbers them.
inline constexpr uint32_t kLeftShift = 0x1, kRightShift = 0x2, kLeftCtrl = 0x4, kRightCtrl = 0x8,
                          kLeftAlt = 0x10, kRightAlt = 0x20;

/// 0x0055d740: `held` less the modifiers the key of a binding begins with. In
/// its script they are not held: ALT-1 bound to an action casts it on the
/// target and not on the player, though ALT is the self cast key.
inline uint32_t modifiersLeftBy(uint32_t held, std::string_view key) {
    struct Row {
        std::string_view name;
        uint32_t bits;
    };
    static constexpr Row kRows[] = {
        {"LSHIFT", kLeftShift}, {"RSHIFT", kRightShift}, {"SHIFT", kLeftShift | kRightShift},
        {"LCTRL", kLeftCtrl},   {"RCTRL", kRightCtrl},   {"CTRL", kLeftCtrl | kRightCtrl},
        {"LALT", kLeftAlt},     {"RALT", kRightAlt},     {"ALT", kLeftAlt | kRightAlt},
    };
    while (!key.empty()) {
        const Row* found = nullptr;
        for (const Row& row : kRows)
            if (key.substr(0, row.name.size()) == row.name) {
                found = &row;
                break;
            }
        if (!found) break;
        held &= ~found->bits;
        key.remove_prefix(found->name.size());
        if (!key.empty() && key.front() == '-') key.remove_prefix(1);
    }
    return held;
}

/// The keys this client reads for itself, every frame, as well as by binding:
/// its movement, the chat box, the game menu and their like. The command's own
/// script does not run on these, or they would be acted on twice; on any other
/// key the player gives the command it does.
inline bool clientReadsKey(std::string_view command, std::string_view key) {
    struct Row {
        std::string_view command, key;
    };
    static constexpr Row kRead[] = {
        {"MOVEFORWARD", "W"},      {"MOVEFORWARD", "UP"},       {"MOVEBACKWARD", "S"},
        {"MOVEBACKWARD", "DOWN"},  {"TURNLEFT", "A"},           {"TURNLEFT", "LEFT"},
        {"TURNRIGHT", "D"},        {"TURNRIGHT", "RIGHT"},      {"STRAFELEFT", "Q"},
        {"STRAFERIGHT", "E"},      {"JUMP", "SPACE"},           {"SITORSTAND", "X"},
        {"TOGGLEAUTORUN", "NUMLOCK"}, {"TOGGLEGAMEMENU", "ESCAPE"}, {"OPENCHAT", "ENTER"},
        {"OPENCHATSLASH", "/"},    {"SCREENSHOT", "PRINTSCREEN"}, {"NAMEPLATES", "V"},
        {"FRIENDNAMEPLATES", "SHIFT-V"}, {"ALLNAMEPLATES", "CTRL-V"},
    };
    for (const Row& row : kRead)
        if (row.command == command && row.key == key) return true;
    return false;
}

}  // namespace wowee::addons
