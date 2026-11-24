#pragma once

// What the interface knows, asked of the functions its own frames ask.
//
// A bag's contents, a quest's objectives, a spell's cooldown: the answer a
// playtest wants is the one the interface is given, so it is asked for where
// the interface asks - in its Lua, with the calls FrameXML makes. Only the
// server's own chunks are run like this, as nobody's code and tainting nothing
// (LuaEngine::query).

#include <string>

#include <nlohmann/json.hpp>

struct lua_State;

namespace wowee::mcp {

using json = nlohmann::json;
struct ToolContext;

/// A Lua value as JSON: a table of 1..n is an array, any other an object.
/// What is neither - a function, a frame - is its type's name. `budget` counts
/// down the values written; past it the rest is left out.
json luaToJson(lua_State* L, int index, int depth, int& budget);

/// The value a chunk returns. Throws ToolError where there is no interface or
/// the chunk raised.
json queryLua(ToolContext& context, const std::string& chunk);
/// The same, null instead of an error.
json tryQueryLua(ToolContext& context, const std::string& chunk);

/// A Lua string literal of any text.
std::string luaQuote(const std::string& text);

/// Text as it is read: colour codes, links' payloads and textures taken out
/// of what a font string is given.
std::string plainText(const std::string& marked);

}  // namespace wowee::mcp
