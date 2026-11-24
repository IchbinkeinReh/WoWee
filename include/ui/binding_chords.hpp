#pragma once

/// The keys a chord is looked for under (0x005622e0): itself, and then with
/// fewer and fewer of its modifiers, up to three, the key alone last.

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::ui {

[[nodiscard]] inline std::vector<std::string> bindingChords(std::string_view chord) {
    std::array<std::string_view, 3> modifiers;
    size_t count = 0;
    // "-" begins no modifier: it is the key itself.
    while (count < modifiers.size()) {
        const size_t dash = chord.find('-');
        if (dash == std::string_view::npos || dash == 0) break;
        modifiers[count++] = chord.substr(0, dash + 1);
        chord.remove_prefix(dash + 1);
    }
    // The order they are tried in, a bit a modifier.
    static constexpr int kOrder[4][8] = {{0}, {1, 0}, {3, 2, 1, 0}, {7, 6, 5, 3, 4, 2, 1, 0}};
    std::vector<std::string> out;
    for (size_t i = 0; i < (size_t{1} << count); ++i) {
        std::string one;
        for (size_t m = 0; m < count; ++m)
            if (kOrder[count][i] & (1 << m)) one += modifiers[m];
        out.push_back(one.append(chord));
    }
    return out;
}

/// A command that is no row of the bindings and runs all the same
/// (0x00562140), in any case.
[[nodiscard]] inline bool bindsDirectly(std::string_view command) {
    for (const std::string_view prefix : {"SPELL ", "ITEM ", "MACRO ", "CLICK "}) {
        if (command.size() < prefix.size()) continue;
        bool same = true;
        for (size_t i = 0; i < prefix.size() && same; ++i)
            same = std::toupper(static_cast<unsigned char>(command[i])) == prefix[i];
        if (same) return true;
    }
    return false;
}

}  // namespace wowee::ui
