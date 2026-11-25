#pragma once

// The kinds of thing the MCP server does not do towards other players - and
// the settings it does not keep on the realm - unless it was started with
// --mcp-allow naming them.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wowee::mcp {

enum class Social : uint32_t {
    Chat    = 1u << 0,   ///< say, yell, party, raid, guild, channels, whispers
    Emote   = 1u << 1,   ///< text emotes, which are read as chat by whoever is near
    Mail    = 1u << 2,
    Trade   = 1u << 3,
    Auction = 1u << 4,
    Group   = 1u << 5,   ///< parties, raids, the dungeon finder, battlegrounds, rolls
    Guild   = 1u << 6,   ///< guilds, charters, arena teams, the calendar
    Duel    = 1u << 7,
    Players = 1u << 8,   ///< spells, items and attacks aimed at another player, pvp
    Friends = 1u << 9,   ///< the friends and ignore lists
    Tickets = 1u << 10,  ///< game master tickets, complaints and bug reports
    /// CVars, key bindings, chat windows and the action bars shown, as the
    /// realm keeps them (settings_ledger.hpp). Not one of `all`.
    Settings = 1u << 11,
};

/// `all`: everything towards other players.
inline constexpr uint32_t kSocialAll = (1u << 11) - 1;
/// Every name --mcp-allow knows.
inline constexpr uint32_t kAllowable = kSocialAll | static_cast<uint32_t>(Social::Settings);

/// "chat", "emote", ...
const char* socialName(Social s);
std::optional<Social> socialNamed(std::string_view name);
/// "chat,mail" or "all" to a mask; nothing when a name in it is not one.
std::optional<uint32_t> parseSocialList(std::string_view list);
/// The names in a mask, comma separated; empty for none.
std::string socialList(uint32_t mask);

}  // namespace wowee::mcp
