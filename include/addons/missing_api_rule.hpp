#pragma once

/// Whether a global nobody set answers with a stand-in (LuaEngine::
/// installMissingApiFallback) instead of nil.
///
/// In the client it is nil, and addons ask by that: `if SomeAddon then`,
/// `MyAddonDB = MyAddonDB or {}`. With the stand-in every one of those read as
/// there, and every addon's saved variables were the one table all of them
/// wrote into. The 3.3.5 interface has all it asks for and needs none; those of
/// the older expansions are loaded with it, as it was brought up with it.

#include <string_view>

namespace wowee::addons {

/// `asked` is WOWEE_LUA_API_FALLBACK, which wins where it says something, and
/// `loads` WOWEE_LOAD_FRAMEXML, on unless it is 0.
[[nodiscard]] constexpr bool missingApiStandsIn(std::string_view asked, std::string_view loads, bool olderExpansion) {
    if (!asked.empty()) return asked != "0";
    return olderExpansion && loads != "0";
}

}  // namespace wowee::addons
