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

/// What the client's getters answer for what is not there: nothing at all, or
/// as many values as ever.
struct NothingAnswer {
    const char* name;
    /// One letter each: n nil, 0 and 1 the numbers, e an empty string, f false,
    /// U the string of UNKNOWN; S for GetSpellLink, whose spell asked for by a
    /// number alone has two nils and any other none.
    const char* values;
};

/// The functions and their answers, as 12340 has them.
std::span<const NothingAnswer> nothingAnswers();

/// Has the functions answer so where their own code answers nothing, nil or
/// false alone.
void answerNothing(lua_State* L, std::span<const NothingAnswer> answers);

}  // namespace wowee::addons
