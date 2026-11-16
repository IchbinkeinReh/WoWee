#pragma once

/**
 * macro_icon_lists.hpp - the two icon lists the macro frame and its
 * neighbours pick from, built as the client builds them (0x00565840).
 *
 * The client walks the files under Interface\Icons\ and keeps two lists:
 * the macro icons - names beginning "Ability_" or "Spell_" (0x00565630,
 * 0x00565550) - and the item icons - names beginning "INV_" (0x005655c0).
 * Only a .blp or .tga file counts, and the name is kept without its
 * extension. Each list starts with INV_Misc_QuestionMark, is sorted without
 * regard to case with the question mark held first (0x00564a20), and has
 * names that differ only in case merged.
 *
 * GetNumMacroIcons / GetMacroIconInfo and SaveEquipmentSet (0x00564d70) use
 * the first; GetNumMacroItemIcons / GetMacroItemIconInfo and the guild bank's
 * tab icons (0x00564d90) the second.
 */

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace wowee {
namespace addons {
namespace macro_icons {

inline constexpr const char* kQuestionMark = "INV_Misc_QuestionMark";

/// strnicmp(a, b, strlen(b)) == 0: `name` begins with `prefix`, any case.
inline bool hasPrefix(const std::string& name, const char* prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i) {
        if (i >= name.size()) return false;
        if (std::tolower(static_cast<unsigned char>(name[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

inline int compareNoCase(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

/// The question mark. The client compares it exactly; the names here can
/// come from a manifest that keeps only lower case, so its case is let go.
inline bool isQuestionMark(const std::string& name) { return compareNoCase(name, kQuestionMark) == 0; }

/// A file name (no folder) to the icon name the lists keep: without its
/// extension, and only for a .blp or a .tga. Empty for anything else.
inline std::string iconName(const std::string& file) {
    const size_t dot = file.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    const std::string ext = file.substr(dot);
    if (compareNoCase(ext, ".blp") != 0 && compareNoCase(ext, ".tga") != 0) return {};
    return file.substr(0, dot);
}

struct Lists {
    std::vector<std::string> macro;  ///< 0x00beaf98: Ability_ and Spell_
    std::vector<std::string> item;   ///< 0x00beafa8: INV_
};

inline void sortAndMerge(std::vector<std::string>& list) {
    std::stable_sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) {
        const bool qa = isQuestionMark(a), qb = isQuestionMark(b);
        if (qa != qb) return qa;
        if (qa) return false;
        return compareNoCase(a, b) < 0;
    });
    list.erase(std::unique(list.begin(), list.end(),
                           [](const std::string& a, const std::string& b) { return compareNoCase(a, b) == 0; }),
               list.end());
}

/// Both lists from the file names under Interface\Icons\ (no folder, with
/// their extension).
inline Lists build(const std::vector<std::string>& files) {
    Lists out;
    out.macro.push_back(kQuestionMark);
    out.item.push_back(kQuestionMark);
    for (const auto& file : files) {
        const std::string name = iconName(file);
        if (name.empty()) continue;
        if (hasPrefix(name, "Ability_") || hasPrefix(name, "Spell_")) out.macro.push_back(name);
        if (hasPrefix(name, "INV_")) out.item.push_back(name);
    }
    sortAndMerge(out.macro);
    sortAndMerge(out.item);
    // The question mark the list began with stays the one kept.
    out.macro.front() = kQuestionMark;
    out.item.front() = kQuestionMark;
    return out;
}

} // namespace macro_icons
} // namespace addons
} // namespace wowee
