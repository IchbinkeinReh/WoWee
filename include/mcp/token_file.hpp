#pragma once

// The token a client of the MCP server shows, and the file it reads it from.
//
// Made anew at every start and written, with the port, to mcp_server.json in
// the user's config root, where only this user can read it; taken away again
// at the end. It is never logged.

#include <cstdint>
#include <filesystem>
#include <string>

namespace wowee::mcp {

/// 32 bytes of the operating system's randomness as 64 hex digits; empty when
/// there is none to be had, and then there is no server.
std::string makeToken();

struct TokenFile {
    uint16_t port = 0;
    std::string token;
    unsigned long pid = 0;
};

/// <config root>/mcp_server.json.
std::filesystem::path tokenFilePath(const std::string& configRoot);
/// Written for this user's eyes only: mode 0600, or on Windows an access list
/// of this user and the system and nothing inherited.
bool writeTokenFile(const std::filesystem::path& path, const TokenFile& file);
/// Only while it is still ours: a client started since has its own in its
/// place.
void removeTokenFile(const std::filesystem::path& path, const std::string& token);

}  // namespace wowee::mcp
