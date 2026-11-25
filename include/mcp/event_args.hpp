#pragma once

/// An event's arguments as the MCP server lists them.

#include "game/handler_types.hpp"

#include <string>
#include <vector>

namespace wowee::mcp {

/// Twenty at most, each cut where it is long; what reaches Lua as nil says so
/// (the engine hands it over as a byte of its own, game::kEventNil).
[[nodiscard]] inline std::vector<std::string> shownArgs(const std::vector<std::string>& args) {
    constexpr size_t kMostArgs = 20;
    constexpr size_t kLongest = 400;
    std::vector<std::string> out;
    for (const auto& a : args) {
        if (out.size() >= kMostArgs) break;
        out.push_back(a == game::kEventNil ? std::string("nil") : a.size() > kLongest ? a.substr(0, kLongest) : a);
    }
    return out;
}

}  // namespace wowee::mcp
