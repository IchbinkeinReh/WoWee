#pragma once

/// The argument of InviteUnit and UninviteUnit: a unit token or a name.
///
/// The original takes either. "target", "focus", "party1", "mouseover" and
/// the rest name a unit, and the unit's name is what is invited; anything
/// else is a player's name as given. This client sent the argument as it
/// came, so InviteUnit("target") invited a player called Target.
///
/// No Lua or game headers, so a test can read it.

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

namespace wowee::addons {

/// `nameOfUnit` answers a lower-cased unit token with that unit's name, or
/// empty where the token names no unit that exists.
[[nodiscard]] inline std::string unitOrName(const std::string& arg,
                                            const std::function<std::string(const std::string&)>& nameOfUnit) {
    std::string token = arg;
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (nameOfUnit) {
        std::string name = nameOfUnit(token);
        if (!name.empty()) return name;
    }
    return arg;
}

}  // namespace wowee::addons
