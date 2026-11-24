#pragma once

/// The argument checks of the client's getters, which their own code here does
/// not make: called with none of what they ask for they raise their usage line
/// (FUN_0084f280 in each), and do not answer nil.

#include <span>

struct lua_State;

namespace wowee::addons {

struct ArgGuard {
    const char* name;
    /// What the arguments are, one letter each: n a number, s a string (a
    /// number is one), t a table, o a table where there is one.
    const char* arguments;
    const char* usage;
};

/// The functions and their lines, as 12340 has them.
std::span<const ArgGuard> argGuards();

/// Puts the checks before the functions; one that is not there is left out.
void guardArguments(lua_State* L, std::span<const ArgGuard> guards);

}  // namespace wowee::addons
