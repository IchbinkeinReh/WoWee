#pragma once

// A font's flags as SetFont and GetFont have them (0x008151a0, 0x008151e0).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace wowee::ui::font_flags {

constexpr uint32_t kOutline = 1, kMonochrome = 2, kThick = 4;

struct Name {
    uint32_t flag;
    const char* name;
};
/// 0x00a44050, in the order they are named in.
inline constexpr Name kNames[] = {{kOutline, "OUTLINE"}, {kThick, "THICKOUTLINE"}, {kMonochrome, "MONOCHROME"}};

/// The flags whose names are anywhere in the text, whatever their case: "THICKOUTLINE"
/// has "OUTLINE" in it.
[[nodiscard]] inline uint32_t parse(std::string_view text) {
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    uint32_t flags = 0;
    for (const Name& n : kNames)
        if (upper.find(n.name) != std::string::npos) flags |= n.flag;
    return flags;
}

/// Their names.
[[nodiscard]] inline std::string names(uint32_t flags) {
    std::string out;
    for (const Name& n : kNames) {
        if ((flags & n.flag) == 0) continue;
        if (!out.empty()) out += ", ";
        out += n.name;
    }
    return out;
}

/// Those of the XML's outline, "NORMAL" or "THICK", and monochrome.
[[nodiscard]] inline uint32_t ofXml(const std::string& outline, bool monochrome) {
    const auto is = [&outline](std::string_view word) {
        return std::equal(outline.begin(), outline.end(), word.begin(), word.end(),
                          [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); });
    };
    const uint32_t outlined = is("NORMAL") ? kOutline : is("THICK") ? kOutline | kThick : 0u;
    return outlined | (monochrome ? kMonochrome : 0u);
}

}  // namespace wowee::ui::font_flags
