#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::addons {

/// A CVar that holds a set of ids - lfdCollapsedHeaders, lfdSelectedDungeons:
/// a character 2, then six ids a character, the lowest first, in its bits over
/// '@' (0x00766860, 0x00766ca0).
[[nodiscard]] inline bool cvarHasId(const std::string& value, uint32_t id) {
    const size_t at = id / 6 + 1;
    return at < value.size() && (value[at] & (1 << (id % 6))) != 0;
}

/// The same with the id put in or taken out; as it was where it would be too long.
[[nodiscard]] inline std::string cvarWithId(std::string value, uint32_t id, bool in) {
    const size_t at = id / 6 + 1;
    if (value.size() > 0xfe || at > 0xfd) return value;
    if (value.empty()) value = " ";
    value[0] = '\x02';
    if (in) {
        if (value.size() <= at) value.resize(at + 1, '@');
        value[at] = static_cast<char>(value[at] | (1 << (id % 6)));
        return value;
    }
    if (at < value.size()) value[at] = static_cast<char>(value[at] & ~(1 << (id % 6)));
    while (value.size() > 1 && value.back() == '@') value.pop_back();
    return value;
}

}  // namespace wowee::addons
