#include "mcp/options.hpp"

#include <cstring>
#include <optional>
#include <string_view>

namespace wowee::mcp {

namespace {

bool allDigits(std::string_view text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

/// A port, 0 to 65535, with nothing but digits in it.
std::optional<uint16_t> parsePort(std::string_view text) {
    if (!allDigits(text) || text.size() > 5) return std::nullopt;
    uint32_t v = 0;
    for (const char c : text) v = v * 10 + static_cast<uint32_t>(c - '0');
    if (v > 65535) return std::nullopt;
    return static_cast<uint16_t>(v);
}

/// What follows "name=" in an argument, nothing when it is another argument.
std::optional<std::string_view> valueOf(std::string_view arg, std::string_view name) {
    if (arg.size() > name.size() && arg.substr(0, name.size()) == name && arg[name.size()] == '=') {
        return arg.substr(name.size() + 1);
    }
    return std::nullopt;
}

}  // namespace

Options parseOptions(int argc, const char* const* argv) {
    Options out;
    bool asked = false;
    const auto takePort = [&](std::string_view text) {
        asked = true;
        if (const auto port = parsePort(text)) out.port = *port;
        else out.problems.push_back("--mcp " + std::string(text) + ": not a port");
    };
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out.help = true;
        } else if (arg == "--mcp") {
            asked = true;
            // A number after it is the port; anything else is the next argument.
            if (i + 1 < argc && argv[i + 1] && allDigits(argv[i + 1])) takePort(argv[++i]);
        } else if (const auto port = valueOf(arg, "--mcp")) {
            takePort(*port);
        } else if (arg == "--mcp-lua") {
            out.lua = true;
        } else if (arg == "--mcp-allow" || valueOf(arg, "--mcp-allow")) {
            std::optional<std::string_view> list = valueOf(arg, "--mcp-allow");
            if (!list && i + 1 < argc && argv[i + 1] && std::strncmp(argv[i + 1], "--", 2) != 0) {
                list = argv[++i];
            }
            const auto mask = list ? parseSocialList(*list) : std::nullopt;
            if (mask) {
                out.allow |= *mask;
            } else {
                out.problems.push_back("--mcp-allow " + std::string(list.value_or("")) +
                                       ": not a list of " + socialList(kSocialAll) + " or all");
            }
        }
    }
    out.enabled = asked && out.problems.empty();
    return out;
}

std::string helpText() {
    return "Usage: wowee [options]\n"
           "\n"
           "  --help, -h           this text\n"
           "  --mcp [PORT]         start the MCP server on 127.0.0.1 (port 8642, or PORT; 0 takes\n"
           "  --mcp=PORT           a free one), for programs that look at the game and play it.\n"
           "                       Off by default and never remembered. docs/MCP_SERVER.md\n"
           "  --mcp-allow=LIST     let the MCP server do things towards other players: any of chat,\n"
           "                       emote, mail, trade, auction, group, guild, duel, players,\n"
           "                       friends, tickets, or all. None by default\n"
           "  --mcp-lua            let the MCP server run Lua in the interface. Off by default\n"
           "\n"
           "Everything else is set in the game or by WOWEE_* environment variables.\n";
}

}  // namespace wowee::mcp
