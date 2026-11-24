#pragma once

#include <string_view>

namespace wowee::addons {

/// Whether an addon's code is the client's own, and runs secure: Blizzard's
/// (0x005f79a0 asks its signature, for which the name stands in here) and this
/// client's, which is a part of it as its windows in C++ are. One of that name
/// among the player's own addons, in the original's folder, is not.
[[nodiscard]] constexpr bool addonIsTheClients(std::string_view name, bool fromPlayersFolder) {
    if (name.substr(0, 9) == "Blizzard_") return true;
    return !fromPlayersFolder && name == "WoweeAllBags";
}

}  // namespace wowee::addons
