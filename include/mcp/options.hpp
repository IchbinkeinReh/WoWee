#pragma once

// What the command line says about the MCP server (docs/MCP_SERVER.md).
//
// Off unless --mcp is given. Nothing here is a setting: a server that lets
// another program play is asked for on every start, by whoever starts the
// client, and never remembered.

#include <cstdint>
#include <string>
#include <vector>

#include "mcp/social.hpp"

namespace wowee::mcp {

/// The port of a plain --mcp. Any free one is taken with --mcp=0; the file
/// the token is written to names it either way (token_file.hpp).
inline constexpr uint16_t kDefaultPort = 8642;

struct Options {
    bool enabled = false;
    uint16_t port = kDefaultPort;
    /// --mcp-lua: the `lua` tool, and /run through the MCP.
    bool lua = false;
    /// --mcp-allow: what may be done towards other players (social.hpp).
    uint32_t allow = 0;
    /// --help / -h.
    bool help = false;
    /// What was asked of the server and could not be understood. With any of
    /// these the server stays off: an option that widens what it may do has
    /// to be read exactly, and a port nobody asked for is no use.
    std::vector<std::string> problems;
};

/// Arguments this does not know are left alone, as the client always has.
Options parseOptions(int argc, const char* const* argv);

/// What --help prints.
std::string helpText();

}  // namespace wowee::mcp
