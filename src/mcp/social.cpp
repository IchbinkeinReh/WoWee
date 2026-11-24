#include "mcp/social.hpp"

#include <array>

namespace wowee::mcp {

namespace {

struct Named {
    Social social;
    const char* name;
};

constexpr std::array<Named, 11> kNames = {{
    {Social::Chat, "chat"},
    {Social::Emote, "emote"},
    {Social::Mail, "mail"},
    {Social::Trade, "trade"},
    {Social::Auction, "auction"},
    {Social::Group, "group"},
    {Social::Guild, "guild"},
    {Social::Duel, "duel"},
    {Social::Players, "players"},
    {Social::Friends, "friends"},
    {Social::Tickets, "tickets"},
}};

}  // namespace

const char* socialName(Social s) {
    for (const auto& n : kNames) {
        if (n.social == s) return n.name;
    }
    return "?";
}

std::optional<Social> socialNamed(std::string_view name) {
    for (const auto& n : kNames) {
        if (name == n.name) return n.social;
    }
    return std::nullopt;
}

std::optional<uint32_t> parseSocialList(std::string_view list) {
    uint32_t mask = 0;
    size_t at = 0;
    while (at <= list.size()) {
        const size_t comma = list.find(',', at);
        const std::string_view one =
            list.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
        if (one == "all") {
            mask |= kSocialAll;
        } else if (const auto s = socialNamed(one)) {
            mask |= static_cast<uint32_t>(*s);
        } else {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    return mask;
}

std::string socialList(uint32_t mask) {
    std::string out;
    for (const auto& n : kNames) {
        if ((mask & static_cast<uint32_t>(n.social)) == 0) continue;
        if (!out.empty()) out += ',';
        out += n.name;
    }
    return out;
}

}  // namespace wowee::mcp
