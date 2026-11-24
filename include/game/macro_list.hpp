#pragma once

/// The macros as the interface numbers them (0x00566240): the account's from
/// 1 and the character's from 37, each half in the order of their names. A
/// macro's own id is another thing - it is what an action slot and the saved
/// list hold, and it stays what it is while macros come, go and are renamed.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game::macro_list {

/// How many of each there may be (0x00a0f82c), and the half a place counts.
inline constexpr uint32_t kAccountMacros = 36;
inline constexpr uint32_t kCharacterMacros = 18;
inline constexpr uint32_t kHalf = 36;
inline constexpr uint32_t kIndexes = 72;   ///< 0x48

struct Entry {
    uint32_t id = 0;   ///< 1..36 the account's, 37..54 the character's
    std::string name;
};

[[nodiscard]] constexpr bool perCharacter(uint32_t id) { return id > kAccountMacros; }

/// As 0x0076e780 compares them: without the case of the ASCII letters.
[[nodiscard]] inline int compareNames(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

/// One half's ids in the interface's order.
[[nodiscard]] inline std::vector<uint32_t> order(const std::vector<Entry>& macros, bool character) {
    std::vector<const Entry*> half;
    for (const auto& m : macros)
        if (perCharacter(m.id) == character) half.push_back(&m);
    std::sort(half.begin(), half.end(), [](const Entry* a, const Entry* b) {
        const int c = compareNames(a->name, b->name);
        return c != 0 ? c < 0 : a->id < b->id;
    });
    std::vector<uint32_t> ids;
    ids.reserve(half.size());
    for (const Entry* m : half) ids.push_back(m->id);
    return ids;
}

/// The macro at an index of the interface's, 0 for none.
[[nodiscard]] inline uint32_t idAt(const std::vector<Entry>& macros, int index) {
    if (index < 1 || index > static_cast<int>(kIndexes)) return 0;
    const bool character = index > static_cast<int>(kHalf);
    const auto ids = order(macros, character);
    const size_t at = static_cast<size_t>(index) - 1 - (character ? kHalf : 0);
    return at < ids.size() ? ids[at] : 0;
}

/// The interface's index of a macro, 0 for one there is none of.
[[nodiscard]] inline int indexOf(const std::vector<Entry>& macros, uint32_t id) {
    const auto ids = order(macros, perCharacter(id));
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id) return static_cast<int>(i + 1 + (perCharacter(id) ? kHalf : 0));
    return 0;
}

/// The macro of a name, without case (0x00566330), 0 for none.
[[nodiscard]] inline uint32_t byName(const std::vector<Entry>& macros, const std::string& name) {
    if (name.empty()) return 0;
    for (const bool character : {false, true})
        for (const uint32_t id : order(macros, character))
            for (const auto& m : macros)
                if (m.id == id && compareNames(m.name, name) == 0) return id;
    return 0;
}

/// An id for a new macro, 0 where the half is full (0x00568020).
[[nodiscard]] inline uint32_t freeId(const std::vector<Entry>& macros, bool character) {
    const uint32_t first = character ? kAccountMacros + 1 : 1;
    const uint32_t last = character ? kAccountMacros + kCharacterMacros : kAccountMacros;
    for (uint32_t id = first; id <= last; ++id) {
        if (std::none_of(macros.begin(), macros.end(), [id](const Entry& m) { return m.id == id; })) return id;
    }
    return 0;
}

}  // namespace wowee::game::macro_list
