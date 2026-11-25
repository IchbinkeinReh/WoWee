#pragma once

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

namespace wowee::addons {

/// A binding's unit argument where 0x0060abf0 is asked with its third argument
/// 1: none, or "", is the target, whoever that is - also nobody (a guid of 0).
/// Else what the token is resolved to, which is nothing where it is no unit's.
template <typename Resolve>
[[nodiscard]] std::optional<uint64_t> optionalUnitArg(lua_State* L, int arg, uint64_t target, Resolve&& resolve) {
    const char* unit = arg > 0 && lua_isstring(L, arg) ? lua_tostring(L, arg) : nullptr;
    if (!unit || !*unit) return target;
    std::string token(unit);
    for (char& c : token) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return resolve(token);
}

}  // namespace wowee::addons
