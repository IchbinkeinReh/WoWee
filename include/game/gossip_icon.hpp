#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wowee {
namespace game {

/// The word GetGossipOptions answers for an option's icon (0x0058a9e0, the
/// table at 0x00acef20): GossipFrame_Update draws
/// Interface\GossipFrame\<word>GossipIcon with it. "spiritguide" and
/// "innkeeper" were not among them, and no such pictures exist.
[[nodiscard]] inline constexpr const char* gossipIconName(uint32_t icon) {
    constexpr std::array<const char*, 11> kNames = {"gossip",  "vendor", "taxi",     "trainer", "healer",       "binder",
                                                    "banker",  "petition", "tabard", "battlemaster", "auctioneer"};
    return icon < kNames.size() ? kNames[static_cast<size_t>(icon)] : "gossip";
}

}  // namespace game
}  // namespace wowee
