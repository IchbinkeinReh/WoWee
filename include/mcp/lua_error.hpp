#pragma once

// A Lua error's text taken apart: whose handler raised it, where, and what it
// says - for the answers of the tools, which give it in parts.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace wowee::mcp {

struct LuaError {
    std::string handler;  ///< "OnEnter on MailItem1Button", "event 'X' handler"; "" where none is named
    std::string source;   ///< a file, or [string "..."] of a chunk's own text; "" where none is named
    int line = 0;         ///< 0 where none is named
    std::string message;
    std::vector<std::string> stack;  ///< "file.lua:12 in name", from where it was raised out
};

/// What an error says of itself ("file.lua:12: message"), with the stack the
/// interface's handler folds in after it ("\n      at file.lua:12 in name").
LuaError parseLuaError(std::string_view text);

/// The error a line of the client's log tells of, where it tells of one
/// ("LuaEngine: OnEnter on X error: file.lua:12: message").
std::optional<LuaError> luaErrorOfLogLine(std::string_view line);

/// {"handler", "source", "line", "message", "stack"}, without what is not
/// known.
nlohmann::json luaErrorJson(const LuaError& error);

}  // namespace wowee::mcp
